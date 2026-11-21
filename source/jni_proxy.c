/* jni_proxy.c -- Java dynamic proxies (Unity's JNIBridge), reflected methods,
 * ClassLoader.findLibrary, and the two Android loopers the engine posts to.
 *
 * PROXIES. Native code implements Java interfaces through
 * bitter/jnibridge/JNIBridge (in the APK):
 *     Object newInterfaceProxy(long ptr, Class[] interfaces)
 *         -> java.lang.reflect.Proxy whose handler, for ANY method called on
 *            it, runs   native invoke(ptr, method.getDeclaringClass(), method, args)
 *            under a lock, or returns null once disableInterfaceProxy() set
 *            ptr to 0.
 * libunity's invoke (libunity+0xa7ec70) turns the Method back into a method
 * ID with JNI FromReflectedMethod and hands (class, methodID, args) to the C++
 * proxy, which compares it against the IDs it got from GetMethodID. So the
 * Method must wrap the interned JMethod of (interface, name, signature) --
 * which is what GetMethodID on that interface returns here. C# AndroidJavaProxy
 * (and so AndroidJavaRunnable) rides the same path, and also reads the name
 * via Method.getName().
 *
 * LOOPERS. Unity runs code on the Android UI thread with Activity.
 * runOnUiThread / new Handler(Looper.getMainLooper()).post, and on its own
 * thread with a Handler bound to that thread's looper. Nothing ran either
 * before, so anything waiting for such a callback waited for ever. Here:
 *   - the UI thread is a real thread (created through the bionic pthread shim
 *     so the Mono GC bridge sees it) draining a time-ordered queue;
 *   - the engine thread (the one in dcr_boot's frame loop, "UnityMain" on a
 *     phone) drains its own queue between frames (jni_looper_run_engine()).
 * Delivery follows Android: runOnUiThread on the UI thread runs at once,
 * everything else is queued.
 *
 * Native proxy objects are never released with JNIBridge.delete: a phone does
 * that from a GC finalizer at some unspecified later time, and doing it on a
 * refcount of ours would risk freeing one that is still in use. They are
 * small and few. MIT.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "bionic.h"
#include "bionic_pthread.h"
#include "config.h"
#include "dcr_path.h"
#include "dcr_jni_unity.h"
#include "so_util.h"
#include "util.h"

int b_pthread_create(b_pthread_t *out, const b_pthread_attr_t *attr, void *(*start)(void *),
                     void *arg); /* bionic_pthread.c */

#define LIB_DIR "/data/app/" DCR_PKG_NAME "-1/lib/arm"
#define PROXY_MAGIC 0x50524F58u /* 'PROX' */
#define MAX_IFACES 8

extern so_module main_mod, unity_mod, mono_mod;

/* ================================ proxies ================================= */
enum { PROXY_JNIBRIDGE, PROXY_CSHARP };

typedef struct {
  uint32_t magic;
  int kind;
  RMutex lock;           /* JNIBridge$a's monitor: reentrant, as in Java */
  jlong ptr;             /* JNIBridge: the native proxy; 0 once disabled */
  jint handle;           /* C#: the AndroidJavaProxy's GC handle */
  int n;
  JClass *ifaces[MAX_IFACES];
} Proxy;

static void proxy_finalize(JObj *o) {
  free(o->p);
  o->p = NULL;
}

int jni_is_proxy(const JObj *o) {
  return o && o->finalize == proxy_finalize && o->p && ((const Proxy *)o->p)->magic == PROXY_MAGIC;
}

/* A java.lang.reflect.Proxy for `ifaces` (a Class or a Class[]). */
static JObj *new_proxy(int kind, jlong ptr, jint handle, const JObj *ifaces) {
  Proxy *p = calloc(1, sizeof *p);
  p->magic = PROXY_MAGIC;
  p->kind = kind;
  rmutexInit(&p->lock);
  p->ptr = ptr;
  p->handle = handle;
  if (ifaces && ifaces->kind == JK_CLASS) {
    p->ifaces[p->n++] = ifaces->c.of;
  } else if (ifaces && ifaces->kind == JK_ARRAY && ifaces->a.elem == 'L') {
    for (jsize i = 0; i < ifaces->a.len && p->n < MAX_IFACES; i++) {
      const JObj *c = ((JObj **)ifaces->a.data)[i];
      if (c && c->kind == JK_CLASS)
        p->ifaces[p->n++] = c->c.of;
    }
  }
  if (!p->n)
    p->ifaces[p->n++] = jni_class("java/lang/Object");
  JObj *o = jni_new(p->ifaces[0]->name); /* instanceof its first interface */
  o->p = p;
  o->finalize = proxy_finalize;
  static int logged;
  if (logged < 32 && ++logged)
    debugPrintf("[proxy] new %s%s (%s %p)%s\n", p->ifaces[0]->name, p->n > 1 ? " +" : "",
                kind == PROXY_CSHARP ? "C# handle" : "native", kind == PROXY_CSHARP
                ? (void *)(uintptr_t)handle : (void *)(uintptr_t)ptr,
                logged == 32 ? " -- not logging further proxies" : "");
  return o;
}

/* JNIBridge.newInterfaceProxy(J[Ljava/lang/Class;)Ljava/lang/Object; */
jvalue jni_h_newInterfaceProxy(JObj *self, const jvalue *a, const JMethod *m) {
  return jv_l(new_proxy(PROXY_JNIBRIDGE, a[0].j, 0, a[1].l));
}

/* JNIBridge.disableInterfaceProxy(Ljava/lang/Object;)V */
jvalue jni_h_disableInterfaceProxy(JObj *self, const jvalue *a, const JMethod *m) {
  JObj *o = a[0].l;
  if (jni_is_proxy(o)) {
    Proxy *p = o->p;
    rmutexLock(&p->lock);
    p->ptr = 0;
    rmutexUnlock(&p->lock);
  }
  return jv_none();
}

static JObj *box(char t, jvalue v) {
  const char *cls = t == 'Z' ? "java/lang/Boolean" : t == 'B' ? "java/lang/Byte"
                  : t == 'C' ? "java/lang/Character" : t == 'S' ? "java/lang/Short"
                  : t == 'I' ? "java/lang/Integer" : t == 'J' ? "java/lang/Long"
                  : t == 'F' ? "java/lang/Float" : "java/lang/Double";
  JObj *o = jni_new(cls);
  switch (t) {
  case 'Z': o->v[0] = v.z; break;
  case 'B': o->v[0] = v.b; break;
  case 'C': o->v[0] = v.c; break;
  case 'S': o->v[0] = v.s; break;
  case 'I': o->v[0] = v.i; break;
  case 'J': memcpy(&o->v[0], &v.j, 8); break;
  case 'F': memcpy(&o->v[0], &v.f, 4); break;
  default: memcpy(&o->v[0], &v.d, 8); break;
  }
  return o;
}

static jvalue unbox(const JObj *o, char t) {
  jvalue r;
  r.j = 0;
  if (t == 'L' || t == '[') {
    r.l = (void *)o;
    return r;
  }
  if (!o || o->kind != JK_OBJECT)
    return r; /* a null for a primitive: a phone would throw; 0 is the kind answer */
  int wide = jni_is(o, "java/lang/Long") || jni_is(o, "java/lang/Double");
  int fl = jni_is(o, "java/lang/Float"), db = jni_is(o, "java/lang/Double");
  switch (t) {
  case 'J': if (wide && !db) memcpy(&r.j, &o->v[0], 8); else r.j = (jlong)o->v[0]; break;
  case 'F': if (fl) memcpy(&r.f, &o->v[0], 4); else r.f = (jfloat)o->v[0]; break;
  case 'D':
    if (db) memcpy(&r.d, &o->v[0], 8);
    else if (fl) { float f; memcpy(&f, &o->v[0], 4); r.d = f; }
    else r.d = (jdouble)o->v[0];
    break;
  default: r.i = (jint)o->v[0]; break;
  }
  return r;
}

/* Called by jni_core's invoke() for every instance method called on a proxy.
 * JNIBridge proxies go to native JNIBridge.invoke(ptr, declaringClass, Method,
 * args); C# AndroidJavaProxy ones (ReflectionHelper$1.invoke) to native
 * ReflectionHelper.nativeProxyInvoke(handle, method.getName(), args). */
jvalue jni_proxy_call(JObj *self, JMethod *m, const jvalue *args) {
  typedef void *(*invoke_fn)(void *env, void *cls, jlong ptr, void *decl, void *method, void *argv);
  typedef void *(*cs_invoke_fn)(void *env, void *cls, jint handle, void *name, void *argv);
  static invoke_fn native;
  static cs_invoke_fn cs_native;
  Proxy *p = self->p;
  if (!native)
    native = (invoke_fn)jni_native("bitter/jnibridge/JNIBridge", "invoke");
  if (!cs_native)
    cs_native = (cs_invoke_fn)jni_native("com/unity3d/player/ReflectionHelper", "nativeProxyInvoke");

  /* Declaring class: one of the proxy's interfaces (java.lang.Object for
   * equals/hashCode/toString, as java.lang.reflect.Proxy does). */
  JClass *decl = NULL;
  for (int i = 0; i < p->n && !decl; i++)
    if (p->ifaces[i] == m->cls)
      decl = m->cls;
  if (!decl)
    decl = (!strcmp(m->name, "equals") || !strcmp(m->name, "hashCode") || !strcmp(m->name, "toString"))
               ? jni_class("java/lang/Object") : p->ifaces[0];
  JMethod *im = decl == m->cls ? m : jni_method(decl, m->name, m->sig, 0);

  /* Arguments as Object[] (null when the method takes none), boxed. */
  int n = 0;
  char types[24];
  for (const char *s = m->sig[0] == '(' ? m->sig + 1 : ""; *s && *s != ')' && n < 24; n++) {
    types[n] = *s;
    while (*s == '[')
      s++;
    s = *s == 'L' ? strchr(s, ';') + 1 : s + 1;
  }
  JObj *argv = NULL;
  if (n) {
    argv = jni_array('L', n);
    for (int i = 0; i < n; i++) {
      char t = types[i];
      JObj *v = (t == 'L' || t == '[') ? jni_retain(args[i].l) : box(t, args[i]);
      ((JObj **)argv->a.data)[i] = v;
    }
  }
  JObj *mobj = jni_new("java/lang/reflect/Method");
  mobj->p = im;

  jvalue r;
  r.j = 0;
  rmutexLock(&p->lock);
  JObj *ret = NULL;
  if (p->kind == PROXY_CSHARP && cs_native) {
    JObj *name = jni_str(m->name);
    ret = cs_native(g_jni_env, jni_class("com/unity3d/player/ReflectionHelper")->obj, p->handle,
                    name, argv);
    jni_release(name);
  } else if (p->kind == PROXY_JNIBRIDGE && p->ptr && native) {
    ret = native(g_jni_env, jni_class("bitter/jnibridge/JNIBridge")->obj, p->ptr, decl->obj, mobj,
                 argv);
  } else if (p->kind == PROXY_CSHARP ? !cs_native : !native) {
    debugPrintf("[proxy] %s.%s called but its native invoke is not registered\n", decl->name,
                m->name);
  }
  r = unbox(ret, m->ret);
  if (m->ret != 'L' && m->ret != '[' && ret)
    jni_release(ret);
  rmutexUnlock(&p->lock);
  jni_release(mobj);
  jni_release(argv);
  return r;
}

/* ======================= java.lang.reflect.Method ========================= */
static const JMethod *meth_of(const JObj *self) {
  const JMethod *jm = self ? self->p : NULL;
  return jm && jm->magic == JMETH_MAGIC ? jm : NULL;
}

/* The Class for one type in a signature: primitives by their Java names. */
static JObj *type_class(const char *s, const char **end) {
  const char *e = s;
  while (*e == '[')
    e++;
  e = *e == 'L' ? strchr(e, ';') + 1 : e + 1;
  if (end)
    *end = e;
  char name[112];
  size_t len = (size_t)(e - s) < sizeof name ? (size_t)(e - s) : sizeof name - 1;
  if (s[0] == 'L') {
    snprintf(name, sizeof name, "%.*s", (int)(len - 2), s + 1);
  } else if (s[0] == '[') {
    snprintf(name, sizeof name, "%.*s", (int)len, s);
  } else {
    static const char *const prim[] = {"Zboolean", "Bbyte", "Cchar", "Sshort", "Iint",
                                       "Jlong", "Ffloat", "Ddouble", "Vvoid", NULL};
    snprintf(name, sizeof name, "void");
    for (int i = 0; prim[i]; i++)
      if (prim[i][0] == s[0])
        snprintf(name, sizeof name, "%s", prim[i] + 1);
  }
  return jni_class(name)->obj;
}

jvalue jni_h_method_getName(JObj *self, const jvalue *a, const JMethod *m) {
  const JMethod *jm = meth_of(self);
  return jv_l(jni_str(jm ? jm->name : ""));
}
jvalue jni_h_method_getDeclaringClass(JObj *self, const jvalue *a, const JMethod *m) {
  const JMethod *jm = meth_of(self);
  return jv_l(jm ? jm->cls->obj : NULL);
}
jvalue jni_h_method_getReturnType(JObj *self, const jvalue *a, const JMethod *m) {
  const JMethod *jm = meth_of(self);
  const char *r = jm ? strchr(jm->sig, ')') : NULL;
  return jv_l(r ? type_class(r + 1, NULL) : NULL);
}
jvalue jni_h_method_getParameterTypes(JObj *self, const jvalue *a, const JMethod *m) {
  const JMethod *jm = meth_of(self);
  const char *s = jm && jm->sig[0] == '(' ? jm->sig + 1 : "";
  JObj *tmp[24];
  int n = 0;
  while (*s && *s != ')' && n < 24)
    tmp[n++] = type_class(s, &s);
  JObj *arr = jni_array('L', n);
  for (int i = 0; i < n; i++)
    ((JObj **)arr->a.data)[i] = jni_retain(tmp[i]);
  return jv_l(arr);
}
jvalue jni_h_method_getModifiers(JObj *self, const jvalue *a, const JMethod *m) {
  const JMethod *jm = meth_of(self);
  return jv_i(jm && jm->is_static ? 0x9 : 0x1); /* public (static) */
}
jvalue jni_h_method_equals(JObj *self, const jvalue *a, const JMethod *m) {
  const JObj *o = a[0].l;
  return jv_z(o && o->kind == JK_OBJECT && jni_is(o, "java/lang/reflect/Method") && o->p == self->p);
}
jvalue jni_h_method_hashCode(JObj *self, const jvalue *a, const JMethod *m) {
  return jv_i((jint)(uintptr_t)self->p);
}
jvalue jni_h_method_toString(JObj *self, const jvalue *a, const JMethod *m) {
  const JMethod *jm = meth_of(self);
  return jv_l(jm ? jni_str_fmt("%s.%s%s", jm->cls->name, jm->name, jm->sig) : jni_str(""));
}

/* ====================== com.unity3d.player.ReflectionHelper ================
 * Unity's C# AndroidJavaObject / AndroidJavaClass / AndroidJavaProxy do every
 * Java lookup through these (in the APK). Java picks the best-matching member
 * for a signature C# builds from its own argument types; here the member is
 * the interned JNI method/field (jni_method_best), the same object JNI
 * GetMethodID/GetFieldID return, so FromReflectedMethod/Field give an ID the
 * handler tables understand. */
jvalue jni_h_rh_getMethodID(JObj *self, const jvalue *a, const JMethod *m) {
  JClass *c = jni_class_of(a[0].l);
  if (!c) {
    jni_throw("java/lang/NullPointerException", "ReflectionHelper.getMethodID: no class");
    return jv_l(NULL);
  }
  JObj *o = jni_new("java/lang/reflect/Method");
  o->p = jni_method_best(c, jni_utf(a[1].l), jni_utf(a[2].l), a[3].z);
  return jv_l(o);
}
jvalue jni_h_rh_getConstructorID(JObj *self, const jvalue *a, const JMethod *m) {
  JClass *c = jni_class_of(a[0].l);
  if (!c) {
    jni_throw("java/lang/NullPointerException", "ReflectionHelper.getConstructorID: no class");
    return jv_l(NULL);
  }
  JObj *o = jni_new("java/lang/reflect/Constructor");
  o->p = jni_method_best(c, "<init>", jni_utf(a[1].l), 0);
  return jv_l(o);
}
jvalue jni_h_rh_getFieldID(JObj *self, const jvalue *a, const JMethod *m) {
  JClass *c = jni_class_of(a[0].l);
  if (!c) {
    jni_throw("java/lang/NullPointerException", "ReflectionHelper.getFieldID: no class");
    return jv_l(NULL);
  }
  JObj *o = jni_new("java/lang/reflect/Field");
  o->p = jni_field(c, jni_utf(a[1].l), jni_utf(a[2].l), a[3].z);
  return jv_l(o);
}
/* newProxyInstance(ILjava/lang/Class;) and (I[Ljava/lang/Class;) */
jvalue jni_h_rh_newProxyInstance(JObj *self, const jvalue *a, const JMethod *m) {
  return jv_l(new_proxy(PROXY_CSHARP, 0, a[0].i, a[1].l));
}

/* ======================== ClassLoader.findLibrary ========================= */
/* Where the app's lib<name>.so lives, or null (as Android answers for one it
 * does not have). Only the libraries this wrapper loads resolve: dlopen
 * (bionic_dl.c) matches these paths by file name. Unity loads Mono this way:
 * findLibrary("mono") then dlopen(path); without it, "Failed to load Mono."  */
jvalue jni_h_findLibrary(JObj *self, const jvalue *a, const JMethod *m) {
  const char *name = jni_utf(a[0].l);
  const so_module *mod = !strcmp(name, "mono") ? &mono_mod : !strcmp(name, "unity") ? &unity_mod
                       : !strcmp(name, "main") ? &main_mod : NULL;
  if (!mod || !mod->load_virtbase) {
    debugPrintf("[jni] ClassLoader.findLibrary(%s) -> null (not part of this port)\n", name);
    return jv_l(NULL);
  }
  JObj *path = jni_str_fmt(LIB_DIR "/lib%s.so", name);
  debugPrintf("[jni] ClassLoader.findLibrary(%s) -> %s\n", name, jni_utf(path));
  return jv_l(path);
}

/* ================================ loopers ================================= */
typedef struct Post {
  JObj *r;               /* a Runnable */
  u64 when;              /* system tick */
  struct Post *next;
} Post;

typedef struct {
  Mutex m;
  CondVar cv;
  Post *head;
  volatile int tid;      /* the thread that drains it; 0 until known */
} Queue;

enum { LOOPER_UI = 0, LOOPER_ENGINE = 1 };
static Queue g_q[2];
static JObj *g_engine_looper;   /* Looper of the frame-loop thread */
static int g_ui_started;
static Mutex g_start_lock;

static void run_runnable(JObj *r) {
  static JMethod *run;
  if (!run)
    run = jni_method(jni_class("java/lang/Runnable"), "run", "()V", 0);
  jni_call(r, run, NULL);
  jni_exception_report("Runnable.run");
}

static void enqueue(int q, JObj *r, u64 delay_ms) {
  Post *p = calloc(1, sizeof *p);
  p->r = jni_retain(r);
  p->when = armGetSystemTick() + armNsToTicks(delay_ms * 1000000ull);
  mutexLock(&g_q[q].m);
  Post **pp = &g_q[q].head;
  while (*pp && (*pp)->when <= p->when)
    pp = &(*pp)->next;
  p->next = *pp;
  *pp = p;
  condvarWakeAll(&g_q[q].cv);
  mutexUnlock(&g_q[q].m);
}

/* Pop the first post that is due; with `wait`, block until one is. */
static JObj *dequeue(int q, int wait) {
  Queue *Q = &g_q[q];
  JObj *r = NULL;
  mutexLock(&Q->m);
  for (;;) {
    u64 now = armGetSystemTick();
    if (Q->head && Q->head->when <= now) {
      Post *p = Q->head;
      Q->head = p->next;
      r = p->r;
      free(p);
      break;
    }
    if (!wait)
      break;
    if (Q->head)
      condvarWaitTimeout(&Q->cv, &Q->m, armTicksToNs(Q->head->when - now));
    else
      condvarWait(&Q->cv, &Q->m);
  }
  mutexUnlock(&Q->m);
  return r;
}

static void *ui_thread(void *arg) {
  g_q[LOOPER_UI].tid = b_gettid();
  debugPrintf("[looper] UI thread running (tid %d)\n", g_q[LOOPER_UI].tid);
  for (;;) {
    JObj *r = dequeue(LOOPER_UI, 1);
    run_runnable(r);
    jni_release(r);
  }
  return NULL;
}

static void ensure_ui_thread(void) {
  if (g_ui_started)
    return;
  mutexLock(&g_start_lock);
  if (!g_ui_started) {
    b_pthread_t t;
    if (b_pthread_create(&t, NULL, ui_thread, NULL) == 0)
      g_ui_started = 1;
    else
      debugPrintf("[looper] could not start the UI thread\n");
  }
  mutexUnlock(&g_start_lock);
}

/* The frame-loop thread: its looper, and the posts due to it. */
void jni_looper_bind_engine(void) {
  g_q[LOOPER_ENGINE].tid = b_gettid();
  if (!g_engine_looper) {
    g_engine_looper = jni_new("android/os/Looper");
    g_engine_looper->immortal = 1;
    g_engine_looper->v[0] = LOOPER_ENGINE + 1;
  }
}

void jni_looper_run_engine(void) {
  JObj *r;
  for (int budget = 64; budget-- && (r = dequeue(LOOPER_ENGINE, 0));) {
    run_runnable(r);
    jni_release(r);
  }
}

static int looper_of(const JObj *looper) {
  return looper && looper->v[0] == LOOPER_ENGINE + 1 ? LOOPER_ENGINE : LOOPER_UI;
}

/* The looper of the calling thread: the engine's on the frame-loop thread,
 * the main (UI) one elsewhere -- a phone would give null on a thread without
 * a looper, but the previous behaviour (always the main looper) is kept for
 * those, since nothing here can run a loop on them. */
static JObj *my_looper(void) {
  int tid = b_gettid();
  if (g_engine_looper && tid == g_q[LOOPER_ENGINE].tid)
    return g_engine_looper;
  return jni_singleton("android/os/Looper");
}

jvalue jni_h_getMainLooper(JObj *self, const jvalue *a, const JMethod *m) {
  return jv_l(jni_singleton("android/os/Looper"));
}
jvalue jni_h_myLooper(JObj *self, const jvalue *a, const JMethod *m) { return jv_l(my_looper()); }

/* Activity.runOnUiThread(Runnable) */
jvalue jni_h_runOnUiThread(JObj *self, const jvalue *a, const JMethod *m) {
  JObj *r = a[0].l;
  if (!r)
    return jv_none();
  if (g_ui_started && b_gettid() == g_q[LOOPER_UI].tid) {
    run_runnable(r);
  } else {
    ensure_ui_thread();
    enqueue(LOOPER_UI, r, 0);
  }
  return jv_none();
}

#define ZV2_HT_MARK 0x5a56 /* v[0] of a Looper/Handler that belongs to a HandlerThread */
struct ZvHandler { struct ZvHT *ht; JObj *looper; JObj *cb; };

/* Handler(...): v[0] = 1 + the looper it posts to. Handler() and
 * Handler(Callback) bind to the calling thread's looper. */
jvalue zv2_ht_handler_init(JObj *self, JObj *looper, JObj *callback); /* below */
int zv2_ht_is_looper(const JObj *looper);
jvalue jni_h_handler_init(JObj *self, const jvalue *a, const JMethod *m) {
  JObj *looper = !strncmp(m->sig, "(Landroid/os/Looper;", 20) ? a[0].l : my_looper();
  if (zv2_ht_is_looper(looper)) {
    /* Handler(Looper, Callback) on a HandlerThread: messages, not Runnables */
    JObj *cb = !strcmp(m->sig, "(Landroid/os/Looper;Landroid/os/Handler$Callback;)V") ? a[1].l : NULL;
    return zv2_ht_handler_init(self, looper, cb);
  }
  self->v[0] = 1 + looper_of(looper);
  return jv_none();
}
static int handler_queue(const JObj *h) {
  return h && h->v[0] == 1 + LOOPER_ENGINE ? LOOPER_ENGINE : LOOPER_UI;
}
jvalue zv2_ht_post(JObj *self, JObj *r, u64 delay_ms);
static jvalue handler_post(JObj *self, JObj *r, u64 delay_ms) {
  if (!r)
    return jv_z(0);
  if (self && self->v[0] == ZV2_HT_MARK)
    return zv2_ht_post(self, r, delay_ms);
  int q = handler_queue(self);
  if (q == LOOPER_UI)
    ensure_ui_thread();
  enqueue(q, r, delay_ms);
  return jv_z(1);
}
jvalue jni_h_handler_post(JObj *self, const jvalue *a, const JMethod *m) {
  return handler_post(self, a[0].l, 0);
}
jvalue jni_h_handler_postDelayed(JObj *self, const jvalue *a, const JMethod *m) {
  /* postDelayed(Runnable, long) and postDelayed(Runnable, Object token, long) */
  jlong ms = !strcmp(m->sig, "(Ljava/lang/Runnable;J)Z") ? a[1].j : a[2].j;
  return handler_post(self, a[0].l, ms > 0 ? (u64)ms : 0);
}
jvalue jni_h_handler_removeCallbacks(JObj *self, const jvalue *a, const JMethod *m) {
  int q = handler_queue(self);
  JObj *r = a[0].l, *drop[16];
  int nd = 0;
  mutexLock(&g_q[q].m);
  for (Post **pp = &g_q[q].head; *pp;) {
    if ((*pp)->r == r && nd < 16) {
      Post *p = *pp;
      *pp = p->next;
      drop[nd++] = p->r;
      free(p);
    } else {
      pp = &(*pp)->next;
    }
  }
  mutexUnlock(&g_q[q].m);
  for (int i = 0; i < nd; i++)
    jni_release(drop[i]);
  return jv_none();
}
jvalue jni_h_handler_getLooper(JObj *self, const jvalue *a, const JMethod *m) {
  if (self && self->v[0] == ZV2_HT_MARK)
    return jv_l(((struct ZvHandler *)self->p)->looper);
  return jv_l(handler_queue(self) == LOOPER_ENGINE && g_engine_looper ? g_engine_looper
                                                                       : jni_singleton("android/os/Looper"));
}

/* The runtime's JNI core asks here before its tables: an instance method
 * called on a java.lang.reflect.Proxy goes to the proxy's handler. */
int port_jni_invoke(JObj *self, JMethod *m, const jvalue *args, jvalue *out) {
  if (m->is_static || !jni_is_proxy(self))
    return 0;
  *out = jni_proxy_call(self, m, args);
  return 1;
}


/* ============================ HandlerThread =============================== *
 * Unity 5.4 runs some native work on a Java HandlerThread: it creates one,
 * builds Handler(thread.getLooper(), callback) with a Handler.Callback proxy
 * of its own, and sends Messages (obtainMessage(what) ... sendToTarget()). The
 * proxy's handleMessage(Message) (native: JNIBridge) does the work, on that
 * thread, and the thread that asked waits for it. Without a thread running the
 * loop nothing is handled and the waiter never wakes (the first frames show,
 * then every thread is parked).
 *
 * A HandlerThread here is a real thread with its own queue of Messages (and
 * Runnables, for Handler.post on its handler), run in order. A Message is an
 * android/os/Message object with the fields `what` and `target`. */
typedef struct ZvItem {
  JObj *msg;             /* a Message, or NULL */
  JObj *run;             /* a Runnable, or NULL */
  JObj *handler;         /* the Handler it was sent through */
  u64 when;
  struct ZvItem *next;
} ZvItem;

typedef struct ZvHT {
  Mutex m;
  CondVar cv;
  ZvItem *head;
  volatile int quit, started, tid;
  char name[32];
  JObj *looper;
} ZvHT;

static ZvHT *ht_of(const JObj *thread_or_looper) { return thread_or_looper ? thread_or_looper->p : NULL; }

int zv2_ht_is_looper(const JObj *looper) { return looper && looper->v[0] == ZV2_HT_MARK; }

static void ht_enqueue(ZvHT *h, JObj *msg, JObj *run, JObj *handler, u64 delay_ms) {
  ZvItem *it = calloc(1, sizeof *it);
  it->msg = msg ? jni_retain(msg) : NULL;
  it->run = run ? jni_retain(run) : NULL;
  it->handler = handler ? jni_retain(handler) : NULL;
  it->when = armGetSystemTick() + armNsToTicks(delay_ms * 1000000ull);
  mutexLock(&h->m);
  ZvItem **pp = &h->head;
  while (*pp && (*pp)->when <= it->when)
    pp = &(*pp)->next;
  it->next = *pp;
  *pp = it;
  condvarWakeAll(&h->cv);
  mutexUnlock(&h->m);
}

static void ht_dispatch(ZvItem *it) {
  if (it->run) {
    run_runnable(it->run);
  } else if (it->msg) {
    static JMethod *handle;
    struct ZvHandler *hd = it->handler ? it->handler->p : NULL;
    if (hd && hd->cb) {
      if (!handle)
        handle = jni_method(jni_class("android/os/Handler$Callback"), "handleMessage",
                            "(Landroid/os/Message;)Z", 0);
      jvalue arg;
      arg.l = it->msg;
      jni_call(hd->cb, handle, &arg);
      jni_exception_report("Handler.Callback.handleMessage");
    } else {
      static int warned;
      if (warned++ < 4)
        debugPrintf("[handlerthread] a message (what %d) for a Handler with no Callback: dropped\n",
                    jni_get_field(it->msg, "what").i);
    }
  }
  if (it->msg) jni_release(it->msg);
  if (it->run) jni_release(it->run);
  if (it->handler) jni_release(it->handler);
}

static void *ht_thread(void *arg) {
  ZvHT *h = arg;
  h->tid = b_gettid();
  debugPrintf("[handlerthread] \"%s\" running (tid %d)\n", h->name, h->tid);
  for (;;) {
    mutexLock(&h->m);
    ZvItem *it = NULL;
    for (;;) {
      u64 now = armGetSystemTick();
      if (h->head && h->head->when <= now) {
        it = h->head;
        h->head = it->next;
        break;
      }
      if (h->quit)
        break;
      if (h->head)
        condvarWaitTimeout(&h->cv, &h->m, armTicksToNs(h->head->when - now));
      else
        condvarWait(&h->cv, &h->m);
    }
    mutexUnlock(&h->m);
    if (!it)
      break;
    ht_dispatch(it);
    free(it);
  }
  debugPrintf("[handlerthread] \"%s\" finished\n", h->name);
  return NULL;
}

/* HandlerThread(String name [, int priority]) */
jvalue zv2_h_ht_init(JObj *self, const jvalue *a, const JMethod *m) {
  ZvHT *h = calloc(1, sizeof *h);
  mutexInit(&h->m);
  condvarInit(&h->cv);
  const char *n = a[0].l ? jni_utf(a[0].l) : "HandlerThread";
  snprintf(h->name, sizeof h->name, "%s", n);
  h->looper = jni_new("android/os/Looper");
  h->looper->v[0] = ZV2_HT_MARK;
  h->looper->p = h;
  self->p = h;
  self->v[0] = ZV2_HT_MARK;
  return jv_none();
}

jvalue zv2_h_ht_start(JObj *self, const jvalue *a, const JMethod *m) {
  if (!self || self->v[0] != ZV2_HT_MARK) /* some other Thread (WWW has its own start) */
    return jv_none();
  ZvHT *h = ht_of(self);
  if (h && !h->started) {
    h->started = 1;
    b_pthread_t t;
    if (b_pthread_create(&t, NULL, ht_thread, h) != 0)
      debugPrintf("[handlerthread] could not start \"%s\"\n", h->name);
  }
  return jv_none();
}

/* HandlerThread.getLooper(): the queue exists before the thread runs it */
jvalue zv2_h_ht_getLooper(JObj *self, const jvalue *a, const JMethod *m) {
  ZvHT *h = self && self->v[0] == ZV2_HT_MARK ? ht_of(self) : NULL;
  return jv_l(h ? jni_retain(h->looper) : NULL);
}

/* HandlerThread.quit/quitSafely, Looper.quit/quitSafely */
jvalue zv2_h_ht_quit(JObj *self, const jvalue *a, const JMethod *m) {
  ZvHT *h = ht_of(self);
  if (h && self->v[0] == ZV2_HT_MARK) {
    mutexLock(&h->m);
    h->quit = 1;
    condvarWakeAll(&h->cv);
    mutexUnlock(&h->m);
  }
  return jv_z(1);
}

jvalue zv2_h_ht_getThreadId(JObj *self, const jvalue *a, const JMethod *m) {
  ZvHT *h = ht_of(self);
  return jv_i(h ? h->tid : -1);
}

static void handler_finalize(JObj *o) {
  struct ZvHandler *hd = o->p;
  if (hd) {
    if (hd->cb) jni_release(hd->cb);
    if (hd->looper) jni_release(hd->looper);
    free(hd);
    o->p = NULL;
  }
}

jvalue zv2_ht_handler_init(JObj *self, JObj *looper, JObj *callback) {
  struct ZvHandler *hd = calloc(1, sizeof *hd);
  hd->ht = ht_of(looper);
  hd->looper = jni_retain(looper);
  hd->cb = callback ? jni_retain(callback) : NULL;
  self->p = hd;
  self->v[0] = ZV2_HT_MARK;
  self->finalize = handler_finalize;
  return jv_none();
}

jvalue zv2_ht_post(JObj *self, JObj *r, u64 delay_ms) {
  struct ZvHandler *hd = self->p;
  if (!hd || !hd->ht)
    return jv_z(0);
  ht_enqueue(hd->ht, NULL, r, self, delay_ms);
  return jv_z(1);
}

/* Handler.obtainMessage(what [, arg1 [, arg2]]) / Message.obtain / new Message */
jvalue zv2_h_obtainMessage(JObj *self, const jvalue *a, const JMethod *m) {
  JObj *msg = jni_new("android/os/Message");
  jni_set_field(msg, "what", jv_i(m->sig[1] == 'I' ? a[0].i : 0), 0);
  jni_set_field(msg, "arg1", jv_i(!strncmp(m->sig, "(II", 3) ? a[1].i : 0), 0);
  jni_set_field(msg, "arg2", jv_i(!strncmp(m->sig, "(III", 4) ? a[2].i : 0), 0);
  jni_set_field(msg, "target", jv_l(jni_retain(self)), 1);
  return jv_l(msg);
}

static jvalue send_msg(JObj *handler, JObj *msg, u64 delay_ms) {
  struct ZvHandler *hd = handler ? handler->p : NULL;
  if (!hd || !hd->ht || !msg)
    return jv_z(0);
  ht_enqueue(hd->ht, msg, NULL, handler, delay_ms);
  return jv_z(1);
}

jvalue zv2_h_sendToTarget(JObj *self, const jvalue *a, const JMethod *m) {
  JObj *target = jni_get_field(self, "target").l;
  send_msg(target, self, 0);
  return jv_none();
}

jvalue zv2_h_getTarget(JObj *self, const jvalue *a, const JMethod *m) {
  JObj *t = jni_get_field(self, "target").l;
  return jv_l(t ? jni_retain(t) : NULL);
}

/* Handler.sendMessage / sendMessageDelayed / sendEmptyMessage */
jvalue zv2_h_sendMessage(JObj *self, const jvalue *a, const JMethod *m) {
  if (!strcmp(m->name, "sendEmptyMessage")) {
    JObj *msg = jni_new("android/os/Message");
    jni_set_field(msg, "what", jv_i(a[0].i), 0);
    jni_set_field(msg, "target", jv_l(jni_retain(self)), 1);
    jvalue r = send_msg(self, msg, 0);
    jni_release(msg);
    return r;
  }
  jni_set_field(a[0].l, "target", jv_l(jni_retain(self)), 1);
  return send_msg(self, a[0].l, !strcmp(m->name, "sendMessageDelayed") && a[1].j > 0 ? (u64)a[1].j : 0);
}
