#include <cstdint>
#include <cmath>
#include <algorithm>

/* Kronos Instruction Set in Cxx */

static const int64_t __true_mask = -1; 
static const int64_t __false_mask = 0;

template <typename T> T _TRUE() {return *(T*)&__true_mask;}
template <typename T> T _FALSE() {return *(T*)&__false_mask;}

int64_t _BITCAST(int64_t t) {return t;}
int32_t _BITCAST(int32_t t) {return t;}
int64_t _BITCAST(double t) {return *(int64_t*)&t;}
int32_t _BITCAST(float t) {return *(int32_t*)&t;}

template <typename T> T _ADD(T a, T b) {return a+b;}
template <typename T> T _MUL(T a, T b) {return a*b;}
template <typename T> T _SUB(T a, T b) {return a-b;}
template <typename T> T _DIV(T a, T b) {return a/b;}
template <typename T> T _CLAMPINDEX(T a, T b) { return a < 0 || a >= b ? 0 : a; }

template <typename T> T _MAX(T a, T b) {return std::max(a,b);}
template <typename T> T _MIN(T a, T b) {return std::min(a,b);}

template <typename T> T _CMPEQ(T a, T b) {return a==b?_TRUE<T>():_FALSE<T>();}
template <typename T> T _CMPNEQ(T a, T b) {return a!=b?_TRUE<T>():_FALSE<T>();}
template <typename T> T _CMPGT(T a, T b) {return a>b?_TRUE<T>():_FALSE<T>();}
template <typename T> T _CMPGE(T a, T b) {return a>=b?_TRUE<T>():_FALSE<T>();}
template <typename T> T _CMPLT(T a, T b) {return a<b?_TRUE<T>():_FALSE<T>();}
template <typename T> T _CMPLE(T a, T b) {return a<=b?_TRUE<T>():_FALSE<T>();}

template <typename T> T _AND(T a, T b) {auto tmp(_BITCAST(a)&_BITCAST(b));return *(T*)&tmp;}
template <typename T> T _ANDNOT(T a, T b) {auto tmp((~_BITCAST(a))&_BITCAST(b));return *(T*)&tmp;}
template <typename T> T _OR(T a, T b) {auto tmp(_BITCAST(a)|_BITCAST(b));return *(T*)&tmp;}
template <typename T> T _XOR(T a, T b) {auto tmp(_BITCAST(a)^_BITCAST(b));return *(T*)&tmp;}

template <typename T> T _BSHIFTL(T a, int b) {auto tmp(_BITCAST(a)<<b);return *(T*)&tmp;}
template <typename T> T _BSHIFTR(T a, int b) {auto tmp(_BITCAST(a)>>b);return *(T*)&tmp;}

template <typename T> T _NEG(T a) {return -a;}
template <typename T> T _ABS(T a) {return a<0?-a:a;}
template <typename T> T _NOT(T a) {auto tmp(~_BITCAST(a));return *(T*)&tmp;}

template <typename T> int32_t _CVTI32(T a) {return (int32_t)a;}
template <typename T> int64_t _CVTI64(T a) {return (int64_t)a;}
template <typename T> float _CVTF32(T a) {return (float)a;}
template <typename T> double _CVTF64(T a) {return (double)a;}

#define FI(FN) static int32_t FN(int32_t a) { return a; } static int64_t FN(int64_t a) {return a;}
FI(_FLOOR) FI(_CEIL) FI(_ROUND) FI(_TRUNC)
#undef FI

static float _SQRT(float a) {return sqrtf(a);}
static double _SQRT(double a) {return sqrt(a);}
static float _FLOOR(float a) {return floorf(a);}
static double _FLOOR(double a) {return floor(a);}
static float _CEIL(float a) {return ceilf(a);}
static double _CEIL(double a) {return ceil(a);}
static float _ROUND(float a) {return a>=0?_FLOOR(a+0.5f):_CEIL(a-0.5f);}
static double _ROUND(double a) {return a>=0?_FLOOR(a+0.5):_CEIL(a-0.5);}
static float _TRUNC(float a) {return a>=0?_FLOOR(a):_CEIL(a);}
static double _TRUNC(double a) {return a>=0?_FLOOR(a):_CEIL(a);}

static float _RCPA(float a) {return 1/a;}
static float _RSQRTA(float a) {return 1/_SQRT(a);}

template <typename T> T _MODULO(T a, T b) { return a - (_FLOOR(a / b) * b); }


template <typename T> T _COS(T x) { return (T)cos(x); }
template <typename T> T _SIN(T x) { return (T)sin(x); }
template <typename T> T _EXP(T x) { return (T)exp(x); }
template <typename T> T _POW(T m, T e) { return (T)pow(m,e); }
template <typename T> T _ATAN2(T m, T e) { return (T)atan2(m, e); }
template <typename T> T _LOG(T x) { return (T)log(x); }
template <typename T> T _LOG10(T x) { return (T)log10(x); }
template <typename T> T _LOG2(T x) { return _LOG(x) / _LOG(2); }