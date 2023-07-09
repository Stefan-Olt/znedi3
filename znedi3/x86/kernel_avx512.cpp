#ifdef ZNEDI3_X86_AVX512

#include <algorithm>
#include <cassert>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <immintrin.h>
#include "ccdep.h"
#include "kernel.h"
#include "kernel_x86.h"

#define mm512_and_ps(a, b) _mm512_castsi512_ps(_mm512_and_si512(_mm512_castps_si512((a)), _mm512_castps_si512((b))))
#define mm512_andnot_ps(a, b) _mm512_castsi512_ps(_mm512_andnot_si512(_mm512_castps_si512((a)), _mm512_castps_si512((b))))
#define mm512_or_ps(a, b) _mm512_castsi512_ps(_mm512_or_si512(_mm512_castps_si512((a)), _mm512_castps_si512((b))))
#define mm512_extractf32x8_ps(a, imm) _mm256_castpd_ps(_mm512_extractf64x4_pd(_mm512_castps_pd((a)), imm))

namespace znedi3 {
namespace {

// Applies a 4x4 transpose to each 128-bit lane.
inline FORCE_INLINE void mm512_transpose4_4x4_ps(__m512 &a, __m512 &b, __m512 &c, __m512 &d)
{
	__m512 t0 = _mm512_shuffle_ps(a, b, 0x44);
	__m512 t1 = _mm512_shuffle_ps(c, d, 0x44);
	__m512 t2 = _mm512_shuffle_ps(a, b, 0xEE);
	__m512 t3 = _mm512_shuffle_ps(c, d, 0xEE);
	a = _mm512_shuffle_ps(t0, t1, 0x88);
	b = _mm512_shuffle_ps(t0, t1, 0xDD);
	c = _mm512_shuffle_ps(t2, t3, 0x88);
	d = _mm512_shuffle_ps(t2, t3, 0xDD);
}

// Applies a 4x4 transpose to each 256-bit lane.
inline FORCE_INLINE void mm512_transpose2_4x4_pd(__m512d &a, __m512d &b, __m512d &c, __m512d &d)
{
	__m512d t0 = _mm512_unpacklo_pd(a, b);
	__m512d t1 = _mm512_unpacklo_pd(c, d);
	__m512d t2 = _mm512_unpackhi_pd(a, b);
	__m512d t3 = _mm512_unpackhi_pd(c, d);
	a = _mm512_permutex2var_pd(t0, _mm512_set_epi64(13, 12, 5, 4, 9, 8, 1, 0), t1);
	b = _mm512_permutex2var_pd(t2, _mm512_set_epi64(13, 12, 5, 4, 9, 8, 1, 0), t3);
	c = _mm512_permutex2var_pd(t0, _mm512_set_epi64(15, 14, 7, 6, 11, 10, 3, 2), t1);
	d = _mm512_permutex2var_pd(t2, _mm512_set_epi64(15, 14, 7, 6, 11, 10, 3, 2), t3);
}

// Transpose a 4x4 matrix of packed 128-bit elements.
inline FORCE_INLINE void mm512_transpose4_ps128(__m512 &a, __m512 &b, __m512 &c, __m512 &d)
{
	__m512 t0 = _mm512_shuffle_f32x4(a, b, 0x44);
	__m512 t1 = _mm512_shuffle_f32x4(c, d, 0x44);
	__m512 t2 = _mm512_shuffle_f32x4(a, b, 0xEE);
	__m512 t3 = _mm512_shuffle_f32x4(c, d, 0xEE);
	a = _mm512_shuffle_f32x4(t0, t1, 0x88);
	b = _mm512_shuffle_f32x4(t0, t1, 0xDD);
	c = _mm512_shuffle_f32x4(t2, t3, 0x88);
	d = _mm512_shuffle_f32x4(t2, t3, 0xDD);
}

inline FORCE_INLINE __m128 mm_rsqrt24_ps(__m128 x)
{
	__m128 tmp0 = _mm_rsqrt_ps(x);
	__m128 tmp1 = _mm_mul_ps(x, tmp0);
	__m128 tmp2 = _mm_mul_ps(_mm_set_ps1(0.5f), tmp0);
	__m128 tmp3 = _mm_fnmadd_ps(tmp1, tmp0, _mm_set_ps1(3.0f));
	return _mm_mul_ps(tmp2, tmp3);
}

inline FORCE_INLINE __m512 mm512_rcp24_ps(__m512 x)
{
	__m512 tmp0 = _mm512_rcp14_ps(x);
	__m512 tmp1 = _mm512_fnmadd_ps(x, tmp0, _mm512_set1_ps(1.0f));
	__m512 tmp2 = _mm512_fmadd_ps(tmp0, tmp1, tmp0);
	return tmp2;
}

inline FORCE_INLINE __m512 mm512_expf_ps(__m512 x)
{
	__m512 i, f;

	x = _mm512_fmadd_ps(_mm512_set1_ps(EXPF_LN2_INV_SCALED), x, _mm512_set1_ps(EXPF_ONE_SCALED));
	// x = _mm512_min_ps(x, _mm512_set1_ps(inf_scaled));
	// x = _mm512_max_ps(x, _mm512_setzero_ps());
	x = _mm512_castsi512_ps(_mm512_cvttps_epi32(x));

	// Clear the mantissa. This represents exp2(floor(x)).
	i = mm512_and_ps(x, _mm512_castsi512_ps(_mm512_set1_epi32(0x7F800000UL)));
	// Reset the exponent to zero. This represents exp2(x - floor(x)).
	f = mm512_and_ps(x, _mm512_castsi512_ps(_mm512_set1_epi32(0x007FFFFFUL)));
	f = mm512_or_ps(f, _mm512_castsi512_ps(_mm512_set1_epi32(0x3F800000UL)));

	x = _mm512_set1_ps(EXP2F_X_PLUS1_REMEZ[4]);
	x = _mm512_fmadd_ps(f, x, _mm512_set1_ps(EXP2F_X_PLUS1_REMEZ[3]));
	x = _mm512_fmadd_ps(f, x, _mm512_set1_ps(EXP2F_X_PLUS1_REMEZ[2]));
	x = _mm512_fmadd_ps(f, x, _mm512_set1_ps(EXP2F_X_PLUS1_REMEZ[1]));
	x = _mm512_fmadd_ps(f, x, _mm512_set1_ps(EXP2F_X_PLUS1_REMEZ[0]));

	return _mm512_mul_ps(i, x);
}

inline FORCE_INLINE __m512 mm512_elliott_ps(__m512 x)
{
	const __m512i mask = _mm512_set1_epi32(UINT32_MAX >> 1);

	__m512 den = mm512_and_ps(x, _mm512_castsi512_ps(mask));
	den = _mm512_add_ps(den, _mm512_set1_ps(1.0f));

	return _mm512_mul_ps(x, mm512_rcp24_ps(den));
}

inline FORCE_INLINE void prescreener_old_layer0_avx512(const float kernel[4][48], const float bias[4], const float * const src[4], ptrdiff_t offset_x,
                                                       float *activation, ptrdiff_t activation_stride, unsigned n)
{
	const ptrdiff_t activation_stride_f = activation_stride / sizeof(float);

	for (ptrdiff_t i = 0; i < static_cast<ptrdiff_t>(n); i += 16) {
		_mm512_store_ps(activation + 0 * activation_stride_f + i, _mm512_setzero_ps());
		_mm512_store_ps(activation + 1 * activation_stride_f + i, _mm512_setzero_ps());
		_mm512_store_ps(activation + 2 * activation_stride_f + i, _mm512_setzero_ps());
		_mm512_store_ps(activation + 3 * activation_stride_f + i, _mm512_setzero_ps());
	}

	// Compute 48x4 convolution.
	for (ptrdiff_t k = 0; k < 4; ++k) {
		const float *window_p = src[k] + offset_x;

		for (ptrdiff_t kk = 0; kk < 12; kk += 4) {
			const __m512 n0_c0 = _mm512_set1_ps(kernel[0][12 * k + kk + 0]);
			const __m512 n0_c1 = _mm512_set1_ps(kernel[0][12 * k + kk + 1]);
			const __m512 n0_c2 = _mm512_set1_ps(kernel[0][12 * k + kk + 2]);
			const __m512 n0_c3 = _mm512_set1_ps(kernel[0][12 * k + kk + 3]);

			const __m512 n1_c0 = _mm512_set1_ps(kernel[1][12 * k + kk + 0]);
			const __m512 n1_c1 = _mm512_set1_ps(kernel[1][12 * k + kk + 1]);
			const __m512 n1_c2 = _mm512_set1_ps(kernel[1][12 * k + kk + 2]);
			const __m512 n1_c3 = _mm512_set1_ps(kernel[1][12 * k + kk + 3]);

			const __m512 n2_c0 = _mm512_set1_ps(kernel[2][12 * k + kk + 0]);
			const __m512 n2_c1 = _mm512_set1_ps(kernel[2][12 * k + kk + 1]);
			const __m512 n2_c2 = _mm512_set1_ps(kernel[2][12 * k + kk + 2]);
			const __m512 n2_c3 = _mm512_set1_ps(kernel[2][12 * k + kk + 3]);

			const __m512 n3_c0 = _mm512_set1_ps(kernel[3][12 * k + kk + 0]);
			const __m512 n3_c1 = _mm512_set1_ps(kernel[3][12 * k + kk + 1]);
			const __m512 n3_c2 = _mm512_set1_ps(kernel[3][12 * k + kk + 2]);
			const __m512 n3_c3 = _mm512_set1_ps(kernel[3][12 * k + kk + 3]);

			for (ptrdiff_t i = 0; i < static_cast<ptrdiff_t>(n); i += 16) {
				__m512 x0 = _mm512_loadu_ps(window_p + i + kk);
				__m512 x4 = _mm512_loadu_ps(window_p + i + kk + 4);

				__m512 x1 = _mm512_castsi512_ps(_mm512_alignr_epi32(_mm512_castps_si512(x4), _mm512_castps_si512(x0), 1));
				__m512 x2 = _mm512_castsi512_ps(_mm512_alignr_epi32(_mm512_castps_si512(x4), _mm512_castps_si512(x0), 2));
				__m512 x3 = _mm512_castsi512_ps(_mm512_alignr_epi32(_mm512_castps_si512(x4), _mm512_castps_si512(x0), 3));

				__m512 accum0 = _mm512_load_ps(activation + 0 * activation_stride_f + i);
				__m512 accum1 = _mm512_load_ps(activation + 1 * activation_stride_f + i);
				__m512 accum2 = _mm512_load_ps(activation + 2 * activation_stride_f + i);
				__m512 accum3 = _mm512_load_ps(activation + 3 * activation_stride_f + i);

				accum0 = _mm512_fmadd_ps(n0_c0, x0, accum0);
				accum0 = _mm512_fmadd_ps(n0_c1, x1, accum0);
				accum0 = _mm512_fmadd_ps(n0_c2, x2, accum0);
				accum0 = _mm512_fmadd_ps(n0_c3, x3, accum0);

				accum1 = _mm512_fmadd_ps(n1_c0, x0, accum1);
				accum1 = _mm512_fmadd_ps(n1_c1, x1, accum1);
				accum1 = _mm512_fmadd_ps(n1_c2, x2, accum1);
				accum1 = _mm512_fmadd_ps(n1_c3, x3, accum1);

				accum2 = _mm512_fmadd_ps(n2_c0, x0, accum2);
				accum2 = _mm512_fmadd_ps(n2_c1, x1, accum2);
				accum2 = _mm512_fmadd_ps(n2_c2, x2, accum2);
				accum2 = _mm512_fmadd_ps(n2_c3, x3, accum2);

				accum3 = _mm512_fmadd_ps(n3_c0, x0, accum3);
				accum3 = _mm512_fmadd_ps(n3_c1, x1, accum3);
				accum3 = _mm512_fmadd_ps(n3_c2, x2, accum3);
				accum3 = _mm512_fmadd_ps(n3_c3, x3, accum3);

				_mm512_store_ps(activation + 0 * activation_stride_f + i, accum0);
				_mm512_store_ps(activation + 1 * activation_stride_f + i, accum1);
				_mm512_store_ps(activation + 2 * activation_stride_f + i, accum2);
				_mm512_store_ps(activation + 3 * activation_stride_f + i, accum3);
			}
		}
	}

	// Add bias and apply elliott function.
	const __m512 bias0 = _mm512_set1_ps(bias[0]);
	const __m512 bias1 = _mm512_set1_ps(bias[1]);
	const __m512 bias2 = _mm512_set1_ps(bias[2]);
	const __m512 bias3 = _mm512_set1_ps(bias[3]);

	for (unsigned i = 0; i < n; i += 16) {
		__m512 n0 = _mm512_load_ps(activation + 0 * activation_stride_f + i);
		__m512 n1 = _mm512_load_ps(activation + 1 * activation_stride_f + i);
		__m512 n2 = _mm512_load_ps(activation + 2 * activation_stride_f + i);
		__m512 n3 = _mm512_load_ps(activation + 3 * activation_stride_f + i);

		n0 = _mm512_add_ps(n0, bias0);
		n1 = _mm512_add_ps(n1, bias1);
		n2 = _mm512_add_ps(n2, bias2);
		n3 = _mm512_add_ps(n3, bias3);

		n1 = mm512_elliott_ps(n1);
		n2 = mm512_elliott_ps(n2);
		n3 = mm512_elliott_ps(n3);

		_mm512_store_ps(activation + 0 * activation_stride_f + i, n0);
		_mm512_store_ps(activation + 1 * activation_stride_f + i, n1);
		_mm512_store_ps(activation + 2 * activation_stride_f + i, n2);
		_mm512_store_ps(activation + 3 * activation_stride_f + i, n3);
	}
}

inline FORCE_INLINE void prescreener_old_layer1_avx512(const float kernel[4][4], const float bias[4], float *activation, ptrdiff_t activation_stride, unsigned n)
{
	const ptrdiff_t activation_stride_f = activation_stride / sizeof(float);

	const __m512 n0_c0 = _mm512_set1_ps(kernel[0][0]);
	const __m512 n0_c1 = _mm512_set1_ps(kernel[0][1]);
	const __m512 n0_c2 = _mm512_set1_ps(kernel[0][2]);
	const __m512 n0_c3 = _mm512_set1_ps(kernel[0][3]);

	const __m512 n1_c0 = _mm512_set1_ps(kernel[1][0]);
	const __m512 n1_c1 = _mm512_set1_ps(kernel[1][1]);
	const __m512 n1_c2 = _mm512_set1_ps(kernel[1][2]);
	const __m512 n1_c3 = _mm512_set1_ps(kernel[1][3]);

	const __m512 n2_c0 = _mm512_set1_ps(kernel[2][0]);
	const __m512 n2_c1 = _mm512_set1_ps(kernel[2][1]);
	const __m512 n2_c2 = _mm512_set1_ps(kernel[2][2]);
	const __m512 n2_c3 = _mm512_set1_ps(kernel[2][3]);

	const __m512 n3_c0 = _mm512_set1_ps(kernel[3][0]);
	const __m512 n3_c1 = _mm512_set1_ps(kernel[3][1]);
	const __m512 n3_c2 = _mm512_set1_ps(kernel[3][2]);
	const __m512 n3_c3 = _mm512_set1_ps(kernel[3][3]);

	const __m512 bias0 = _mm512_set1_ps(bias[0]);
	const __m512 bias1 = _mm512_set1_ps(bias[1]);
	const __m512 bias2 = _mm512_set1_ps(bias[2]);
	const __m512 bias3 = _mm512_set1_ps(bias[3]);

	// Compute 4x4 convolution.
	for (unsigned i = 0; i < n; i += 16) {
		__m512 x0 = _mm512_load_ps(activation + 0 * activation_stride_f + i);
		__m512 x1 = _mm512_load_ps(activation + 1 * activation_stride_f + i);
		__m512 x2 = _mm512_load_ps(activation + 2 * activation_stride_f + i);
		__m512 x3 = _mm512_load_ps(activation + 3 * activation_stride_f + i);

		__m512 accum0 = _mm512_fmadd_ps(n0_c0, x0, bias0);
		__m512 accum1 = _mm512_fmadd_ps(n1_c0, x0, bias1);
		__m512 accum2 = _mm512_fmadd_ps(n2_c0, x0, bias2);
		__m512 accum3 = _mm512_fmadd_ps(n3_c0, x0, bias3);

		accum0 = _mm512_fmadd_ps(n0_c1, x1, accum0);
		accum1 = _mm512_fmadd_ps(n1_c1, x1, accum1);
		accum2 = _mm512_fmadd_ps(n2_c1, x1, accum2);
		accum3 = _mm512_fmadd_ps(n3_c1, x1, accum3);

		accum0 = _mm512_fmadd_ps(n0_c2, x2, accum0);
		accum1 = _mm512_fmadd_ps(n1_c2, x2, accum1);
		accum2 = _mm512_fmadd_ps(n2_c2, x2, accum2);
		accum3 = _mm512_fmadd_ps(n3_c2, x2, accum3);

		accum0 = _mm512_fmadd_ps(n0_c3, x3, accum0);
		accum1 = _mm512_fmadd_ps(n1_c3, x3, accum1);
		accum2 = _mm512_fmadd_ps(n2_c3, x3, accum2);
		accum3 = _mm512_fmadd_ps(n3_c3, x3, accum3);

		accum0 = mm512_elliott_ps(accum0);
		accum1 = mm512_elliott_ps(accum1);
		accum2 = mm512_elliott_ps(accum2);
		accum3 = mm512_elliott_ps(accum3);

		_mm512_store_ps(activation + 4 * activation_stride_f + i, accum0);
		_mm512_store_ps(activation + 5 * activation_stride_f + i, accum1);
		_mm512_store_ps(activation + 6 * activation_stride_f + i, accum2);
		_mm512_store_ps(activation + 7 * activation_stride_f + i, accum3);
	}
}

inline FORCE_INLINE void prescreener_old_layer2_avx512(const float kernel[4][8], const float bias[4], float *activation, ptrdiff_t activation_stride,
                                                       unsigned char *prescreen, unsigned n)
{
	const ptrdiff_t activation_stride_f = activation_stride / sizeof(float);

	__m512 n0_c0, n0_c1, n0_c2, n0_c3, n0_c4, n0_c5, n0_c6, n0_c7;
	__m512 n1_c0, n1_c1, n1_c2, n1_c3, n1_c4, n1_c5, n1_c6, n1_c7;
	__m512 bias0, bias1;

	// Evaluate and collapse neurons 0 and 1.
	n0_c0 = _mm512_set1_ps(kernel[0][0]);
	n0_c1 = _mm512_set1_ps(kernel[0][1]);
	n0_c2 = _mm512_set1_ps(kernel[0][2]);
	n0_c3 = _mm512_set1_ps(kernel[0][3]);
	n0_c4 = _mm512_set1_ps(kernel[0][4]);
	n0_c5 = _mm512_set1_ps(kernel[0][5]);
	n0_c6 = _mm512_set1_ps(kernel[0][6]);
	n0_c7 = _mm512_set1_ps(kernel[0][7]);

	n1_c0 = _mm512_set1_ps(kernel[1][0]);
	n1_c1 = _mm512_set1_ps(kernel[1][1]);
	n1_c2 = _mm512_set1_ps(kernel[1][2]);
	n1_c3 = _mm512_set1_ps(kernel[1][3]);
	n1_c4 = _mm512_set1_ps(kernel[1][4]);
	n1_c5 = _mm512_set1_ps(kernel[1][5]);
	n1_c6 = _mm512_set1_ps(kernel[1][6]);
	n1_c7 = _mm512_set1_ps(kernel[1][7]);

	bias0 = _mm512_set1_ps(bias[0]);
	bias1 = _mm512_set1_ps(bias[1]);

	// Compute 2x8 convolution.
	for (unsigned i = 0; i < n; i += 16) {
		__m512 x0 = _mm512_load_ps(activation + 0 * activation_stride_f + i);
		__m512 x1 = _mm512_load_ps(activation + 1 * activation_stride_f + i);
		__m512 x2 = _mm512_load_ps(activation + 2 * activation_stride_f + i);
		__m512 x3 = _mm512_load_ps(activation + 3 * activation_stride_f + i);
		__m512 x4 = _mm512_load_ps(activation + 4 * activation_stride_f + i);
		__m512 x5 = _mm512_load_ps(activation + 5 * activation_stride_f + i);
		__m512 x6 = _mm512_load_ps(activation + 6 * activation_stride_f + i);
		__m512 x7 = _mm512_load_ps(activation + 7 * activation_stride_f + i);

		__m512 accum0a = _mm512_fmadd_ps(n0_c0, x0, bias0);
		__m512 accum1a = _mm512_fmadd_ps(n1_c0, x0, bias1);
		__m512 accum0b = _mm512_mul_ps(n0_c1, x1);
		__m512 accum1b = _mm512_mul_ps(n1_c1, x1);

		accum0a = _mm512_fmadd_ps(n0_c2, x2, accum0a);
		accum1a = _mm512_fmadd_ps(n1_c2, x2, accum1a);

		accum0b = _mm512_fmadd_ps(n0_c3, x3, accum0b);
		accum1b = _mm512_fmadd_ps(n1_c3, x3, accum1b);

		accum0a = _mm512_fmadd_ps(n0_c4, x4, accum0a);
		accum1a = _mm512_fmadd_ps(n1_c4, x4, accum1a);

		accum0b = _mm512_fmadd_ps(n0_c5, x5, accum0b);
		accum1b = _mm512_fmadd_ps(n1_c5, x5, accum1b);

		accum0a = _mm512_fmadd_ps(n0_c6, x6, accum0a);
		accum1a = _mm512_fmadd_ps(n1_c6, x6, accum1a);

		accum0b = _mm512_fmadd_ps(n0_c7, x7, accum0b);
		accum1b = _mm512_fmadd_ps(n1_c7, x7, accum1b);

		accum0a = _mm512_add_ps(accum0a, accum0b);
		accum1a = _mm512_add_ps(accum1a, accum1b);

		accum0a = _mm512_max_ps(accum0a, accum1a);
		_mm512_store_ps(activation + 8 * activation_stride_f + i, accum0a);
	}

	// Evaluate and collapse neurons 2 and 3
	n0_c0 = _mm512_set1_ps(kernel[2][0]);
	n0_c1 = _mm512_set1_ps(kernel[2][1]);
	n0_c2 = _mm512_set1_ps(kernel[2][2]);
	n0_c3 = _mm512_set1_ps(kernel[2][3]);
	n0_c4 = _mm512_set1_ps(kernel[2][4]);
	n0_c5 = _mm512_set1_ps(kernel[2][5]);
	n0_c6 = _mm512_set1_ps(kernel[2][6]);
	n0_c7 = _mm512_set1_ps(kernel[2][7]);

	n1_c0 = _mm512_set1_ps(kernel[3][0]);
	n1_c1 = _mm512_set1_ps(kernel[3][1]);
	n1_c2 = _mm512_set1_ps(kernel[3][2]);
	n1_c3 = _mm512_set1_ps(kernel[3][3]);
	n1_c4 = _mm512_set1_ps(kernel[3][4]);
	n1_c5 = _mm512_set1_ps(kernel[3][5]);
	n1_c6 = _mm512_set1_ps(kernel[3][6]);
	n1_c7 = _mm512_set1_ps(kernel[3][7]);

	bias0 = _mm512_set1_ps(bias[2]);
	bias1 = _mm512_set1_ps(bias[3]);

	// Compute 2x8 convolution.
	for (unsigned i = 0; i < n; i += 16) {
		__m512 x0 = _mm512_load_ps(activation + 0 * activation_stride_f + i);
		__m512 x1 = _mm512_load_ps(activation + 1 * activation_stride_f + i);
		__m512 x2 = _mm512_load_ps(activation + 2 * activation_stride_f + i);
		__m512 x3 = _mm512_load_ps(activation + 3 * activation_stride_f + i);
		__m512 x4 = _mm512_load_ps(activation + 4 * activation_stride_f + i);
		__m512 x5 = _mm512_load_ps(activation + 5 * activation_stride_f + i);
		__m512 x6 = _mm512_load_ps(activation + 6 * activation_stride_f + i);
		__m512 x7 = _mm512_load_ps(activation + 7 * activation_stride_f + i);

		__m512 accum0a = _mm512_fmadd_ps(n0_c0, x0, bias0);
		__m512 accum1a = _mm512_fmadd_ps(n1_c0, x0, bias1);
		__m512 accum0b = _mm512_mul_ps(n0_c1, x1);
		__m512 accum1b = _mm512_mul_ps(n1_c1, x1);

		accum0a = _mm512_fmadd_ps(n0_c2, x2, accum0a);
		accum1a = _mm512_fmadd_ps(n1_c2, x2, accum1a);

		accum0b = _mm512_fmadd_ps(n0_c3, x3, accum0b);
		accum1b = _mm512_fmadd_ps(n1_c3, x3, accum1b);

		accum0a = _mm512_fmadd_ps(n0_c4, x4, accum0a);
		accum1a = _mm512_fmadd_ps(n1_c4, x4, accum1a);

		accum0b = _mm512_fmadd_ps(n0_c5, x5, accum0b);
		accum1b = _mm512_fmadd_ps(n1_c5, x5, accum1b);

		accum0a = _mm512_fmadd_ps(n0_c6, x6, accum0a);
		accum1a = _mm512_fmadd_ps(n1_c6, x6, accum1a);

		accum0b = _mm512_fmadd_ps(n0_c7, x7, accum0b);
		accum1b = _mm512_fmadd_ps(n1_c7, x7, accum1b);

		accum0a = _mm512_add_ps(accum0a, accum0b);
		accum1a = _mm512_add_ps(accum1a, accum1b);

		accum0a = _mm512_max_ps(accum0a, accum1a);

		__m512 activation89 = _mm512_load_ps(activation + 8 * activation_stride_f + i);
		__mmask16 result = _mm512_cmp_ps_mask(accum0a, activation89, _CMP_LE_OQ);

		__m128i prescreen_mask = _mm512_maskz_cvtusepi32_epi8(result, _mm512_set1_epi32(0xFFFFFFFFUL));
		_mm_store_si128((__m128i *)(prescreen + i), prescreen_mask);
	}
}


class PrescreenerOldAVX512F final : public Prescreener {
	PrescreenerOldCoefficients m_data;
public:
	PrescreenerOldAVX512F(const PrescreenerOldCoefficients &data, double half) :
		m_data(data)
	{
		subtract_mean(m_data, half);
	}

	size_t get_tmp_size() const noexcept override
	{
		return 9 * 512 * sizeof(float);
	}

	void process(const float * const src[4], unsigned char *prescreen, void *tmp, unsigned n) const noexcept override
	{
		float *activation = static_cast<float *>(tmp);
		ptrdiff_t activation_stride = 512 * sizeof(float);

		ptrdiff_t window_offset = 5;

		for (ptrdiff_t i = 0; i < static_cast<ptrdiff_t>(n); i += 512) {
			unsigned nn = static_cast<unsigned>(i + 512 > static_cast<ptrdiff_t>(n) ? static_cast<ptrdiff_t>(n) - i : 512);

			prescreener_old_layer0_avx512(m_data.kernel_l0, m_data.bias_l0, src, i - window_offset, activation, activation_stride, nn);
			prescreener_old_layer1_avx512(m_data.kernel_l1, m_data.bias_l1, activation, activation_stride, nn);
			prescreener_old_layer2_avx512(m_data.kernel_l2, m_data.bias_l2, activation, activation_stride, prescreen + i, nn);
		}
	}
};


class PrescreenerNewAVX512F final : public Prescreener {
	AlignedVector<PrescreenerNewCoefficients> m_data;
public:
	PrescreenerNewAVX512F(const PrescreenerNewCoefficients &data, double half) :
		m_data(1, data)
	{
		subtract_mean(m_data[0], half);
	}

	size_t get_tmp_size() const noexcept override { return 0; }

	void process(const float * const src[4], unsigned char *prescreen, void *, unsigned n) const noexcept override
	{
		ptrdiff_t window_offset = 6;

		const float *srcp0 = src[0];
		const float *srcp1 = src[1];
		const float *srcp2 = src[2];
		const float *srcp3 = src[3];

		const __m512 l0_c00 = _mm512_load_ps(m_data[0].kernel_l0[0] + 0);
		const __m512 l0_c01 = _mm512_load_ps(m_data[0].kernel_l0[0] + 16);
		const __m512 l0_c02 = _mm512_load_ps(m_data[0].kernel_l0[0] + 32);
		const __m512 l0_c03 = _mm512_load_ps(m_data[0].kernel_l0[0] + 48);

		const __m512 l0_c10 = _mm512_load_ps(m_data[0].kernel_l0[1] + 0);
		const __m512 l0_c11 = _mm512_load_ps(m_data[0].kernel_l0[1] + 16);
		const __m512 l0_c12 = _mm512_load_ps(m_data[0].kernel_l0[1] + 32);
		const __m512 l0_c13 = _mm512_load_ps(m_data[0].kernel_l0[1] + 48);

		const __m512 l0_c20 = _mm512_load_ps(m_data[0].kernel_l0[2] + 0);
		const __m512 l0_c21 = _mm512_load_ps(m_data[0].kernel_l0[2] + 16);
		const __m512 l0_c22 = _mm512_load_ps(m_data[0].kernel_l0[2] + 32);
		const __m512 l0_c23 = _mm512_load_ps(m_data[0].kernel_l0[2] + 48);

		const __m512 l0_c30 = _mm512_load_ps(m_data[0].kernel_l0[3] + 0);
		const __m512 l0_c31 = _mm512_load_ps(m_data[0].kernel_l0[3] + 16);
		const __m512 l0_c32 = _mm512_load_ps(m_data[0].kernel_l0[3] + 32);
		const __m512 l0_c33 = _mm512_load_ps(m_data[0].kernel_l0[3] + 48);

		for (ptrdiff_t j = 0; j < static_cast<ptrdiff_t>(n); j += 16) {
			// Layer 1.
			__m512 x0, x1, x2, x3;
			__m512 partial0, partial1, partial2, partial3;
			__m512 tmp0, tmp1, tmp2, tmp3;

			// Pixels [0-3].
			x0 = _mm512_loadu_ps(srcp0 - window_offset + j + 0);
			x1 = _mm512_loadu_ps(srcp1 - window_offset + j + 0);
			x2 = _mm512_loadu_ps(srcp2 - window_offset + j + 0);
			x3 = _mm512_loadu_ps(srcp3 - window_offset + j + 0);

			tmp0 = _mm512_mul_ps(l0_c00, x0);
			tmp0 = _mm512_fmadd_ps(l0_c01, x1, tmp0);
			tmp0 = _mm512_fmadd_ps(l0_c02, x2, tmp0);
			tmp0 = _mm512_fmadd_ps(l0_c03, x3, tmp0);

			tmp1 = _mm512_mul_ps(l0_c10, x0);
			tmp1 = _mm512_fmadd_ps(l0_c11, x1, tmp1);
			tmp1 = _mm512_fmadd_ps(l0_c12, x2, tmp1);
			tmp1 = _mm512_fmadd_ps(l0_c13, x3, tmp1);

			tmp2 = _mm512_mul_ps(l0_c20, x0);
			tmp2 = _mm512_fmadd_ps(l0_c21, x1, tmp2);
			tmp2 = _mm512_fmadd_ps(l0_c22, x2, tmp2);
			tmp2 = _mm512_fmadd_ps(l0_c23, x3, tmp2);

			tmp3 = _mm512_mul_ps(l0_c30, x0);
			tmp3 = _mm512_fmadd_ps(l0_c31, x1, tmp3);
			tmp3 = _mm512_fmadd_ps(l0_c32, x2, tmp3);
			tmp3 = _mm512_fmadd_ps(l0_c33, x3, tmp3);

			mm512_transpose4_4x4_ps(tmp0, tmp1, tmp2, tmp3);
			tmp0 = _mm512_add_ps(tmp0, tmp1);
			tmp2 = _mm512_add_ps(tmp2, tmp3);
			partial0 = _mm512_add_ps(tmp0, tmp2);

			// Pixels [4-7].
			x0 = _mm512_loadu_ps(srcp0 - window_offset + j + 4);
			x1 = _mm512_loadu_ps(srcp1 - window_offset + j + 4);
			x2 = _mm512_loadu_ps(srcp2 - window_offset + j + 4);
			x3 = _mm512_loadu_ps(srcp3 - window_offset + j + 4);

			tmp0 = _mm512_mul_ps(l0_c00, x0);
			tmp0 = _mm512_fmadd_ps(l0_c01, x1, tmp0);
			tmp0 = _mm512_fmadd_ps(l0_c02, x2, tmp0);
			tmp0 = _mm512_fmadd_ps(l0_c03, x3, tmp0);

			tmp1 = _mm512_mul_ps(l0_c10, x0);
			tmp1 = _mm512_fmadd_ps(l0_c11, x1, tmp1);
			tmp1 = _mm512_fmadd_ps(l0_c12, x2, tmp1);
			tmp1 = _mm512_fmadd_ps(l0_c13, x3, tmp1);

			tmp2 = _mm512_mul_ps(l0_c20, x0);
			tmp2 = _mm512_fmadd_ps(l0_c21, x1, tmp2);
			tmp2 = _mm512_fmadd_ps(l0_c22, x2, tmp2);
			tmp2 = _mm512_fmadd_ps(l0_c23, x3, tmp2);

			tmp3 = _mm512_mul_ps(l0_c30, x0);
			tmp3 = _mm512_fmadd_ps(l0_c31, x1, tmp3);
			tmp3 = _mm512_fmadd_ps(l0_c32, x2, tmp3);
			tmp3 = _mm512_fmadd_ps(l0_c33, x3, tmp3);

			mm512_transpose4_4x4_ps(tmp0, tmp1, tmp2, tmp3);
			tmp0 = _mm512_add_ps(tmp0, tmp1);
			tmp2 = _mm512_add_ps(tmp2, tmp3);
			partial1 = _mm512_add_ps(tmp0, tmp2);

			// Pixels [8-11].
			x0 = _mm512_loadu_ps(srcp0 - window_offset + j + 8);
			x1 = _mm512_loadu_ps(srcp1 - window_offset + j + 8);
			x2 = _mm512_loadu_ps(srcp2 - window_offset + j + 8);
			x3 = _mm512_loadu_ps(srcp3 - window_offset + j + 8);

			tmp0 = _mm512_mul_ps(l0_c00, x0);
			tmp0 = _mm512_fmadd_ps(l0_c01, x1, tmp0);
			tmp0 = _mm512_fmadd_ps(l0_c02, x2, tmp0);
			tmp0 = _mm512_fmadd_ps(l0_c03, x3, tmp0);

			tmp1 = _mm512_mul_ps(l0_c10, x0);
			tmp1 = _mm512_fmadd_ps(l0_c11, x1, tmp1);
			tmp1 = _mm512_fmadd_ps(l0_c12, x2, tmp1);
			tmp1 = _mm512_fmadd_ps(l0_c13, x3, tmp1);

			tmp2 = _mm512_mul_ps(l0_c20, x0);
			tmp2 = _mm512_fmadd_ps(l0_c21, x1, tmp2);
			tmp2 = _mm512_fmadd_ps(l0_c22, x2, tmp2);
			tmp2 = _mm512_fmadd_ps(l0_c23, x3, tmp2);

			tmp3 = _mm512_mul_ps(l0_c30, x0);
			tmp3 = _mm512_fmadd_ps(l0_c31, x1, tmp3);
			tmp3 = _mm512_fmadd_ps(l0_c32, x2, tmp3);
			tmp3 = _mm512_fmadd_ps(l0_c33, x3, tmp3);

			mm512_transpose4_4x4_ps(tmp0, tmp1, tmp2, tmp3);
			tmp0 = _mm512_add_ps(tmp0, tmp1);
			tmp2 = _mm512_add_ps(tmp2, tmp3);
			partial2 = _mm512_add_ps(tmp0, tmp2);

			// Pixels [12-15].
			x0 = _mm512_loadu_ps(srcp0 - window_offset + j + 12);
			x1 = _mm512_loadu_ps(srcp1 - window_offset + j + 12);
			x2 = _mm512_loadu_ps(srcp2 - window_offset + j + 12);
			x3 = _mm512_loadu_ps(srcp3 - window_offset + j + 12);

			tmp0 = _mm512_mul_ps(l0_c00, x0);
			tmp0 = _mm512_fmadd_ps(l0_c01, x1, tmp0);
			tmp0 = _mm512_fmadd_ps(l0_c02, x2, tmp0);
			tmp0 = _mm512_fmadd_ps(l0_c03, x3, tmp0);

			tmp1 = _mm512_mul_ps(l0_c10, x0);
			tmp1 = _mm512_fmadd_ps(l0_c11, x1, tmp1);
			tmp1 = _mm512_fmadd_ps(l0_c12, x2, tmp1);
			tmp1 = _mm512_fmadd_ps(l0_c13, x3, tmp1);

			tmp2 = _mm512_mul_ps(l0_c20, x0);
			tmp2 = _mm512_fmadd_ps(l0_c21, x1, tmp2);
			tmp2 = _mm512_fmadd_ps(l0_c22, x2, tmp2);
			tmp2 = _mm512_fmadd_ps(l0_c23, x3, tmp2);

			tmp3 = _mm512_mul_ps(l0_c30, x0);
			tmp3 = _mm512_fmadd_ps(l0_c31, x1, tmp3);
			tmp3 = _mm512_fmadd_ps(l0_c32, x2, tmp3);
			tmp3 = _mm512_fmadd_ps(l0_c33, x3, tmp3);

			mm512_transpose4_4x4_ps(tmp0, tmp1, tmp2, tmp3);
			tmp0 = _mm512_add_ps(tmp0, tmp1);
			tmp2 = _mm512_add_ps(tmp2, tmp3);
			partial3 = _mm512_add_ps(tmp0, tmp2);

			// Finish summing neurons.
			mm512_transpose4_ps128(partial0, partial1, partial2, partial3);
			partial0 = _mm512_add_ps(partial0, partial1);
			partial2 = _mm512_add_ps(partial2, partial3);
			partial0 = _mm512_add_ps(partial0, partial2);

			__m512 activation_l0 = _mm512_add_ps(partial0, _mm512_broadcast_f32x4(_mm_load_ps(m_data[0].bias_l0)));
			activation_l0 = mm512_elliott_ps(activation_l0);

			// Layer 2.
			tmp0 = _mm512_mul_ps(_mm512_broadcast_f32x4(_mm_load_ps(m_data[0].kernel_l1[0])), activation_l0);
			tmp1 = _mm512_mul_ps(_mm512_broadcast_f32x4(_mm_load_ps(m_data[0].kernel_l1[1])), activation_l0);
			tmp2 = _mm512_mul_ps(_mm512_broadcast_f32x4(_mm_load_ps(m_data[0].kernel_l1[2])), activation_l0);
			tmp3 = _mm512_mul_ps(_mm512_broadcast_f32x4(_mm_load_ps(m_data[0].kernel_l1[3])), activation_l0);

			mm512_transpose4_4x4_ps(tmp0, tmp1, tmp2, tmp3);
			tmp0 = _mm512_add_ps(tmp0, tmp1);
			tmp2 = _mm512_add_ps(tmp2, tmp3);
			tmp0 = _mm512_add_ps(tmp0, tmp2);

			__m512 activation_l1 = _mm512_add_ps(tmp0, _mm512_broadcast_f32x4(_mm_load_ps(m_data[0].bias_l1)));
			__mmask16 result = _mm512_cmp_ps_mask(activation_l1, _mm512_setzero_ps(), _CMP_GT_OQ);
			__m128i result_mask = _mm512_maskz_cvtusepi32_epi8(result, _mm512_set1_epi8(0xFFU));
			_mm_store_si128((__m128i *)(prescreen + j), result_mask);
		}
	}
};


inline FORCE_INLINE void gather_pixels_avx512(const float * const *src, ptrdiff_t offset_x, ptrdiff_t xdim, ptrdiff_t ydim, float *buf, __m512d *partial_sum_sumsq)
{
	__m512d sum0 = _mm512_setzero_pd();
	__m512d sum1 = _mm512_setzero_pd();
	__m512d sumsq0 = _mm512_setzero_pd();
	__m512d sumsq1 = _mm512_setzero_pd();

	{
		const float *srcp0 = src[0];
		const float *srcp1 = src[1];
		const float *srcp2 = src[2];
		const float *srcp3 = src[3];

		for (ptrdiff_t j = 0; j < xdim; j += 8) {
			{
				__m256 val0 = _mm256_loadu_ps(srcp0 + offset_x + j);
				__m256 val1 = _mm256_loadu_ps(srcp1 + offset_x + j);

				__m512d vald0 = _mm512_cvtps_pd(val0);
				__m512d vald1 = _mm512_cvtps_pd(val1);

				sum0 = _mm512_add_pd(sum0, vald0);
				sum1 = _mm512_add_pd(sum1, vald1);

				sumsq0 = _mm512_fmadd_pd(vald0, vald0, sumsq0);
				sumsq1 = _mm512_fmadd_pd(vald1, vald1, sumsq1);

				_mm256_store_ps(buf + 0 * xdim + j, val0);
				_mm256_store_ps(buf + 1 * xdim + j, val1);
			}
			{
				__m256 val0 = _mm256_loadu_ps(srcp2 + offset_x + j);
				__m256 val1 = _mm256_loadu_ps(srcp3 + offset_x + j);

				__m512d vald0 = _mm512_cvtps_pd(val0);
				__m512d vald1 = _mm512_cvtps_pd(val1);

				sum0 = _mm512_add_pd(sum0, vald0);
				sum1 = _mm512_add_pd(sum1, vald1);

				sumsq0 = _mm512_fmadd_pd(vald0, vald0, sumsq0);
				sumsq1 = _mm512_fmadd_pd(vald1, vald1, sumsq1);

				_mm256_store_ps(buf + 2 * xdim + j, val0);
				_mm256_store_ps(buf + 3 * xdim + j, val1);
			}
		}
		buf += 4 * xdim;
	}

	if (ydim > 4) {
		const float *srcp0 = src[4];
		const float *srcp1 = src[5];

		for (ptrdiff_t j = 0; j < xdim; j += 8) {
			__m256 val0 = _mm256_loadu_ps(srcp0 + offset_x + j);
			__m256 val1 = _mm256_loadu_ps(srcp1 + offset_x + j);

			__m512d vald0 = _mm512_cvtps_pd(val0);
			__m512d vald1 = _mm512_cvtps_pd(val1);

			sum0 = _mm512_add_pd(sum0, vald0);
			sum1 = _mm512_add_pd(sum1, vald1);

			sumsq0 = _mm512_fmadd_pd(vald0, vald0, sumsq0);
			sumsq1 = _mm512_fmadd_pd(vald1, vald1, sumsq1);

			_mm256_store_ps(buf + 0 * xdim + j, val0);
			_mm256_store_ps(buf + 1 * xdim + j, val1);
		}
		buf += 2 * xdim;
	}

	partial_sum_sumsq[0] = _mm512_add_pd(sum0, sum1);
	partial_sum_sumsq[1] = _mm512_add_pd(sumsq0, sumsq1);
}

inline FORCE_INLINE void input_stddev_x4(const __m512d *partial_sum_sumsq, float *mstd, double inv_size)
{
	__m512d partial_sum0 = partial_sum_sumsq[0];
	__m512d partial_sum1 = partial_sum_sumsq[2];
	__m512d partial_sum2 = partial_sum_sumsq[4];
	__m512d partial_sum3 = partial_sum_sumsq[6];

	__m512d partial_sumsq0 = partial_sum_sumsq[1];
	__m512d partial_sumsq1 = partial_sum_sumsq[3];
	__m512d partial_sumsq2 = partial_sum_sumsq[5];
	__m512d partial_sumsq3 = partial_sum_sumsq[7];

	mm512_transpose2_4x4_pd(partial_sum0, partial_sum1, partial_sum2, partial_sum3);
	mm512_transpose2_4x4_pd(partial_sumsq0, partial_sumsq1, partial_sumsq2, partial_sumsq3);

	partial_sum0 = _mm512_add_pd(partial_sum0, partial_sum1);
	partial_sum2 = _mm512_add_pd(partial_sum2, partial_sum3);
	partial_sum0 = _mm512_add_pd(partial_sum0, partial_sum2);

	partial_sumsq0 = _mm512_add_pd(partial_sumsq0, partial_sumsq1);
	partial_sumsq2 = _mm512_add_pd(partial_sumsq2, partial_sumsq3);
	partial_sumsq0 = _mm512_add_pd(partial_sumsq0, partial_sumsq2);

	__m256d sum = _mm256_add_pd(_mm512_castpd512_pd256(partial_sum0), _mm512_extractf64x4_pd(partial_sum0, 1));
	__m256d sumsq = _mm256_add_pd(_mm512_castpd512_pd256(partial_sumsq0), _mm512_extractf64x4_pd(partial_sumsq0, 1));

	 sum = _mm256_mul_pd(sum, _mm256_set1_pd(inv_size));
	 sumsq = _mm256_mul_pd(sumsq, _mm256_set1_pd(inv_size));

	__m256d variance = _mm256_fnmadd_pd(sum, sum, sumsq);
	__mmask8 epsilon_mask = _mm512_cmp_pd_mask(_mm512_castpd256_pd512(variance), _mm512_set1_pd(FLT_EPSILON), _CMP_GE_OQ);

	__m128 variance_f32 = _mm256_cvtpd_ps(variance);
	__m128 stddev_inv = mm_rsqrt24_ps(variance_f32);
	__m128 stddev = _mm_mul_ps(stddev_inv, variance_f32);

	stddev_inv = _mm512_castps512_ps128(_mm512_maskz_mov_ps(epsilon_mask, _mm512_castps128_ps512(stddev_inv)));
	stddev = _mm512_castps512_ps128(_mm512_maskz_mov_ps(epsilon_mask, _mm512_castps128_ps512(stddev)));

	__m128 mstd0 = _mm256_cvtpd_ps(sum);
	__m128 mstd1 = stddev;
	__m128 mstd2 = stddev_inv;
	__m128 mstd3 = _mm_setzero_ps();

	_mm_store_ps(mstd + 0 * 4, mstd0);
	_mm_store_ps(mstd + 1 * 4, mstd1);
	_mm_store_ps(mstd + 2 * 4, mstd2);
	_mm_store_ps(mstd + 3 * 4, mstd3);
}

template <unsigned Step>
inline FORCE_INLINE void sgemv_x4_avx512(const float *matrix, const float *vector, const float *bias, unsigned matrix_rows, unsigned matrix_cols,
                                         float *result, unsigned nns, const float *scale)
{
	static_assert(Step == 32 || Step == 64, "bad step");

	float *activation_softmax = result;
	float *activation_elliott = result + 4 * static_cast<ptrdiff_t>(nns);

	for (ptrdiff_t i = 0; i < static_cast<ptrdiff_t>(matrix_rows); i += Step) {
		__m512 accum0a = _mm512_setzero_ps();
		__m512 accum1a = _mm512_setzero_ps();
		__m512 accum2a = _mm512_setzero_ps();
		__m512 accum3a = _mm512_setzero_ps();

		__m512 accum0b = _mm512_setzero_ps();
		__m512 accum1b = _mm512_setzero_ps();
		__m512 accum2b = _mm512_setzero_ps();
		__m512 accum3b = _mm512_setzero_ps();

		__m512 accum0c = _mm512_setzero_ps();
		__m512 accum1c = _mm512_setzero_ps();
		__m512 accum2c = _mm512_setzero_ps();
		__m512 accum3c = _mm512_setzero_ps();

		__m512 accum0d = _mm512_setzero_ps();
		__m512 accum1d = _mm512_setzero_ps();
		__m512 accum2d = _mm512_setzero_ps();
		__m512 accum3d = _mm512_setzero_ps();

		for (ptrdiff_t j = 0; j < static_cast<ptrdiff_t>(matrix_cols); ++j) {
			__m512 x0 = _mm512_set1_ps(vector[0 * static_cast<ptrdiff_t>(matrix_cols) + j]);
			__m512 x1 = _mm512_set1_ps(vector[1 * static_cast<ptrdiff_t>(matrix_cols) + j]);
			__m512 x2 = _mm512_set1_ps(vector[2 * static_cast<ptrdiff_t>(matrix_cols) + j]);
			__m512 x3 = _mm512_set1_ps(vector[3 * static_cast<ptrdiff_t>(matrix_cols) + j]);
			__m512 coeffs;

			coeffs = _mm512_load_ps(matrix + j * matrix_rows + i + 0);

			accum0a = _mm512_fmadd_ps(coeffs, x0, accum0a);
			accum1a = _mm512_fmadd_ps(coeffs, x1, accum1a);
			accum2a = _mm512_fmadd_ps(coeffs, x2, accum2a);
			accum3a = _mm512_fmadd_ps(coeffs, x3, accum3a);

			coeffs = _mm512_load_ps(matrix + j * matrix_rows + i + 16);

			accum0b = _mm512_fmadd_ps(coeffs, x0, accum0b);
			accum1b = _mm512_fmadd_ps(coeffs, x1, accum1b);
			accum2b = _mm512_fmadd_ps(coeffs, x2, accum2b);
			accum3b = _mm512_fmadd_ps(coeffs, x3, accum3b);

			if (Step >= 64) {
				coeffs = _mm512_load_ps(matrix + j * matrix_rows + i + 32);

				accum0c = _mm512_fmadd_ps(coeffs, x0, accum0c);
				accum1c = _mm512_fmadd_ps(coeffs, x1, accum1c);
				accum2c = _mm512_fmadd_ps(coeffs, x2, accum2c);
				accum3c = _mm512_fmadd_ps(coeffs, x3, accum3c);

				coeffs = _mm512_load_ps(matrix + j * matrix_rows + i + 48);

				accum0d = _mm512_fmadd_ps(coeffs, x0, accum0d);
				accum1d = _mm512_fmadd_ps(coeffs, x1, accum1d);
				accum2d = _mm512_fmadd_ps(coeffs, x2, accum2d);
				accum3d = _mm512_fmadd_ps(coeffs, x3, accum3d);
			}
		}

		__m512 scale0 = _mm512_set1_ps(scale[0]);
		__m512 scale1 = _mm512_set1_ps(scale[1]);
		__m512 scale2 = _mm512_set1_ps(scale[2]);
		__m512 scale3 = _mm512_set1_ps(scale[3]);
		__m512 bias_ps;
		float *dst;

		bias_ps = _mm512_load_ps(bias + i + 0);
		accum0a = _mm512_fmadd_ps(scale0, accum0a, bias_ps);
		accum1a = _mm512_fmadd_ps(scale1, accum1a, bias_ps);
		accum2a = _mm512_fmadd_ps(scale2, accum2a, bias_ps);
		accum3a = _mm512_fmadd_ps(scale3, accum3a, bias_ps);

		dst = i + 0 >= static_cast<ptrdiff_t>(nns) ? activation_elliott + (i + 0) - static_cast<ptrdiff_t>(nns) : activation_softmax + i + 0;
		_mm512_store_ps(dst + 0 * static_cast<ptrdiff_t>(nns), accum0a);
		_mm512_store_ps(dst + 1 * static_cast<ptrdiff_t>(nns), accum1a);
		_mm512_store_ps(dst + 2 * static_cast<ptrdiff_t>(nns), accum2a);
		_mm512_store_ps(dst + 3 * static_cast<ptrdiff_t>(nns), accum3a);

		bias_ps = _mm512_load_ps(bias + i + 16);
		accum0b = _mm512_fmadd_ps(scale0, accum0b, bias_ps);
		accum1b = _mm512_fmadd_ps(scale1, accum1b, bias_ps);
		accum2b = _mm512_fmadd_ps(scale2, accum2b, bias_ps);
		accum3b = _mm512_fmadd_ps(scale3, accum3b, bias_ps);

		dst = i + 16 >= static_cast<ptrdiff_t>(nns) ? activation_elliott + (i + 16) - static_cast<ptrdiff_t>(nns) : activation_softmax + i + 16;
		_mm512_store_ps(dst + 0 * static_cast<ptrdiff_t>(nns), accum0b);
		_mm512_store_ps(dst + 1 * static_cast<ptrdiff_t>(nns), accum1b);
		_mm512_store_ps(dst + 2 * static_cast<ptrdiff_t>(nns), accum2b);
		_mm512_store_ps(dst + 3 * static_cast<ptrdiff_t>(nns), accum3b);

		if (Step >= 64) {
			bias_ps = _mm512_load_ps(bias + i + 32);
			accum0c = _mm512_fmadd_ps(scale0, accum0c, bias_ps);
			accum1c = _mm512_fmadd_ps(scale1, accum1c, bias_ps);
			accum2c = _mm512_fmadd_ps(scale2, accum2c, bias_ps);
			accum3c = _mm512_fmadd_ps(scale3, accum3c, bias_ps);

			dst = i + 32 >= static_cast<ptrdiff_t>(nns) ? activation_elliott + (i + 32) - static_cast<ptrdiff_t>(nns) : activation_softmax + i + 32;
			_mm512_store_ps(dst + 0 * static_cast<ptrdiff_t>(nns), accum0c);
			_mm512_store_ps(dst + 1 * static_cast<ptrdiff_t>(nns), accum1c);
			_mm512_store_ps(dst + 2 * static_cast<ptrdiff_t>(nns), accum2c);
			_mm512_store_ps(dst + 3 * static_cast<ptrdiff_t>(nns), accum3c);

			bias_ps = _mm512_load_ps(bias + i + 48);
			accum0d = _mm512_fmadd_ps(scale0, accum0d, bias_ps);
			accum1d = _mm512_fmadd_ps(scale1, accum1d, bias_ps);
			accum2d = _mm512_fmadd_ps(scale2, accum2d, bias_ps);
			accum3d = _mm512_fmadd_ps(scale3, accum3d, bias_ps);

			dst = i + 48 >= static_cast<ptrdiff_t>(nns) ? activation_elliott + (i + 48) - static_cast<ptrdiff_t>(nns) : activation_softmax + i + 48;
			_mm512_store_ps(dst + 0 * static_cast<ptrdiff_t>(nns), accum0d);
			_mm512_store_ps(dst + 1 * static_cast<ptrdiff_t>(nns), accum1d);
			_mm512_store_ps(dst + 2 * static_cast<ptrdiff_t>(nns), accum2d);
			_mm512_store_ps(dst + 3 * static_cast<ptrdiff_t>(nns), accum3d);
		}
	}
}

inline FORCE_INLINE void softmax_exp(float *ptr, unsigned n)
{
	const __m512 abs_mask = _mm512_castsi512_ps(_mm512_set1_epi32(UINT32_MAX >> 1));
	const __m512 exp_max = _mm512_set1_ps(80.0f);

	for (unsigned i = 0; i < n; i += 16) {
		__m512 x = _mm512_load_ps(ptr + i);
		__m512 xabs = mm512_and_ps(abs_mask, x);
		__m512 xsign = mm512_andnot_ps(abs_mask, x);
		x = _mm512_min_ps(xabs, exp_max);
		x = mm512_or_ps(xsign, x);
		x = mm512_expf_ps(x);
		_mm512_store_ps(ptr + i, x);
	}
}

inline FORCE_INLINE void wae5_x4(const float *softmax, const float *elliott, unsigned n, float *mstd)
{
	__m512 vsum0 = _mm512_setzero_ps();
	__m512 vsum1 = _mm512_setzero_ps();
	__m512 vsum2 = _mm512_setzero_ps();
	__m512 vsum3 = _mm512_setzero_ps();

	__m512 wsum0 = _mm512_setzero_ps();
	__m512 wsum1 = _mm512_setzero_ps();
	__m512 wsum2 = _mm512_setzero_ps();
	__m512 wsum3 = _mm512_setzero_ps();

	for (ptrdiff_t i = 0; i < static_cast<ptrdiff_t>(n); i += 16) {
		__m512 s0 = _mm512_load_ps(softmax + 0 * static_cast<ptrdiff_t>(n) + i);
		__m512 s1 = _mm512_load_ps(softmax + 1 * static_cast<ptrdiff_t>(n) + i);
		__m512 s2 = _mm512_load_ps(softmax + 2 * static_cast<ptrdiff_t>(n) + i);
		__m512 s3 = _mm512_load_ps(softmax + 3 * static_cast<ptrdiff_t>(n) + i);

		__m512 e0 = _mm512_load_ps(elliott + 0 * static_cast<ptrdiff_t>(n) + i);
		__m512 e1 = _mm512_load_ps(elliott + 1 * static_cast<ptrdiff_t>(n) + i);
		__m512 e2 = _mm512_load_ps(elliott + 2 * static_cast<ptrdiff_t>(n) + i);
		__m512 e3 = _mm512_load_ps(elliott + 3 * static_cast<ptrdiff_t>(n) + i);

		e0 = mm512_elliott_ps(e0);
		e1 = mm512_elliott_ps(e1);
		e2 = mm512_elliott_ps(e2);
		e3 = mm512_elliott_ps(e3);

		vsum0 = _mm512_fmadd_ps(s0, e0, vsum0);
		vsum1 = _mm512_fmadd_ps(s1, e1, vsum1);
		vsum2 = _mm512_fmadd_ps(s2, e2, vsum2);
		vsum3 = _mm512_fmadd_ps(s3, e3, vsum3);

		wsum0 = _mm512_add_ps(wsum0, s0);
		wsum1 = _mm512_add_ps(wsum1, s1);
		wsum2 = _mm512_add_ps(wsum2, s2);
		wsum3 = _mm512_add_ps(wsum3, s3);
	}

	mm512_transpose4_4x4_ps(vsum0, vsum1, vsum2, vsum3);
	vsum0 = _mm512_add_ps(vsum0, vsum1);
	vsum2 = _mm512_add_ps(vsum2, vsum3);
	vsum0 = _mm512_add_ps(vsum0, vsum2);

	__m256 vsum_reduced_256 = _mm256_add_ps(_mm512_castps512_ps256(vsum0), mm512_extractf32x8_ps(vsum0, 1));
	__m128 vsum_reduced = _mm_add_ps(_mm256_castps256_ps128(vsum_reduced_256), _mm256_extractf128_ps(vsum_reduced_256, 1));

	mm512_transpose4_4x4_ps(wsum0, wsum1, wsum2, wsum3);
	wsum0 = _mm512_add_ps(wsum0, wsum1);
	wsum2 = _mm512_add_ps(wsum2, wsum3);
	wsum0 = _mm512_add_ps(wsum0, wsum2);

	__m256 wsum_reduced_256 = _mm256_add_ps(_mm512_castps512_ps256(wsum0), mm512_extractf32x8_ps(wsum0, 1));
	__m128 wsum_reduced = _mm_add_ps(_mm256_castps256_ps128(wsum_reduced_256), _mm256_extractf128_ps(wsum_reduced_256, 1));

	__m128 mask = _mm_cmp_ps(wsum_reduced, _mm_set_ps1(1e-10f), _CMP_GT_OQ);

	// Gather mstd[0] and mstd[1].
	__m128 mstd0 = _mm_load_ps(mstd + 0 * 4);
	__m128 mstd1 = _mm_load_ps(mstd + 1 * 4);
	__m128 mstd3 = _mm_load_ps(mstd + 3 * 4);

	vsum_reduced = _mm_mul_ps(vsum_reduced, _mm_set_ps1(5.0f));
	vsum_reduced = _mm_div_ps(vsum_reduced, wsum_reduced);
	vsum_reduced = _mm_fmadd_ps(mstd1, vsum_reduced, mstd0);
	vsum_reduced = _mm_blendv_ps(mstd0, vsum_reduced, mask);

	mstd3 = _mm_add_ps(mstd3, vsum_reduced);
	_mm_store_ps(mstd + 3 * 4, mstd3);
}


template <unsigned N>
class PredictorAVX512F final : public Predictor {
	InterleavedPredictorModel m_model;
	double m_inv_filter_size;
	bool m_use_q2;

	void apply_model(const float *input, float *activation, float *mstd, const __m512d *partial_sum_sumsq) const
	{
		unsigned filter_size = m_model.xdim * m_model.ydim;
		unsigned nns = m_model.nns;

		float *activation_softmax = activation;
		float *activation_elliott = activation + 4 * nns;

		for (unsigned q = 0; q < (m_use_q2 ? 2U : 1U); ++q) {
			const float *neurons = q ? m_model.neurons_q2 : m_model.neurons_q1;
			const float *bias = q ? m_model.bias_q2 : m_model.bias_q1;

			input_stddev_x4(partial_sum_sumsq, mstd, m_inv_filter_size);
			sgemv_x4_avx512<N>(neurons, input, bias, nns * 2, filter_size, activation, nns, mstd + 2 * 4);
			softmax_exp(activation_softmax, 4 * nns);
			wae5_x4(activation_softmax, activation_elliott, nns, mstd);
		}
	}
public:
	PredictorAVX512F(const PredictorModel &model, bool use_q2) :
		m_model(create_interleaved_predictor_model(model)),
		m_inv_filter_size{ 1.0 / (m_model.xdim * m_model.ydim) },
		m_use_q2{ use_q2 }
	{
		assert(model.first.xdim * model.first.ydim <= 48 * 6);
	}

	size_t get_tmp_size() const noexcept override
	{
		FakeAllocator alloc;

		alloc.allocate_n<float>(48 * 6 * 4);
		alloc.allocate_n<float>(256 * 2 * 4);
		alloc.allocate_n<float>(4 * 4);

		return alloc.count();
	}

	void process(const float * const src[6], float *dst, const unsigned char *prescreen, void *tmp, unsigned n) const noexcept override
	{
		LinearAllocator alloc{ tmp };

		ptrdiff_t window_offset_y = 3 - (m_model.ydim / 2);
		ptrdiff_t window_offset_x = static_cast<size_t>(m_model.xdim) / 2 - 1;
		size_t filter_size = static_cast<size_t>(m_model.xdim) * m_model.ydim;

		float *input = alloc.allocate_n<float>(48 * 6 * 4);
		float *activation = alloc.allocate_n<float>(256 * 2 * 4);
		float *mstd = alloc.allocate_n<float>(4 * 4);

		__m512d partial_sum_sumsq[8];
		unsigned gathered_idx[4];
		size_t num_gathered = 0;

		for (ptrdiff_t i = 0; i < static_cast<ptrdiff_t>(n); ++i) {
			if (prescreen[i])
				continue;

			gather_pixels_avx512(src + window_offset_y, i - window_offset_x, m_model.xdim, m_model.ydim, input + num_gathered * filter_size, partial_sum_sumsq + num_gathered * 2);
			gathered_idx[num_gathered] = static_cast<unsigned>(i);
			++num_gathered;

			if (num_gathered == 4) {
				apply_model(input, activation, mstd, partial_sum_sumsq);

				dst[gathered_idx[0]] = mstd[3 * 4 + 0];
				dst[gathered_idx[1]] = mstd[3 * 4 + 1];
				dst[gathered_idx[2]] = mstd[3 * 4 + 2];
				dst[gathered_idx[3]] = mstd[3 * 4 + 3];

				num_gathered = 0;
			}
		}
		if (num_gathered) {
			apply_model(input, activation, mstd, partial_sum_sumsq);

			for (ptrdiff_t idx = 0; idx < static_cast<ptrdiff_t>(num_gathered); ++idx) {
				dst[gathered_idx[idx]] = mstd[3 * 4 + idx];
			}
		}
	}
};

} // namespace


void cubic_interpolation_avx512f(const float * const src[4], float *dst, const unsigned char *prescreen, unsigned n)
{
	const float *srcp0 = src[0];
	const float *srcp1 = src[1];
	const float *srcp2 = src[2];
	const float *srcp3 = src[3];

	const __m512 k0 = _mm512_set1_ps(-3.0f / 32.0f);
	const __m512 k1 = _mm512_set1_ps(19.0f / 32.0f);

	for (unsigned i = 0; i < n; i += 16) {
		__m512i pmask = _mm512_cvtepi8_epi32(_mm_load_si128((const __m128i *)(prescreen + i)));
		__mmask16 mask = _mm512_cmp_epi32_mask(pmask, _mm512_setzero_si512(), _MM_CMPINT_NE);

		__m512 accum = _mm512_maskz_mul_ps(mask, k0, _mm512_load_ps(srcp0 + i));
		accum = _mm512_maskz_fmadd_ps(mask, k1, _mm512_load_ps(srcp1 + i), accum);
		accum = _mm512_maskz_fmadd_ps(mask, k1, _mm512_load_ps(srcp2 + i), accum);
		accum = _mm512_maskz_fmadd_ps(mask, k0, _mm512_load_ps(srcp3 + i), accum);

		_mm512_mask_store_ps(dst + i, mask, accum);
	}
}

void byte_to_float_avx512f(const void *src, void *dst, size_t n)
{
	const uint8_t *src_p = static_cast<const uint8_t *>(src);
	float *dst_p = static_cast<float *>(dst);

	for (size_t i = 0; i < n - n % 16; i += 16) {
		__m512i x = _mm512_cvtepu8_epi32(_mm_load_si128((const __m128i *)(src_p + i)));
		_mm512_store_ps(dst_p + i, _mm512_cvtepi32_ps(x));
	}
	if (n % 16) {
		__m512i x = _mm512_cvtepu8_epi32(_mm_load_si128((const __m128i *)(src_p + (n - n % 16))));
		_mm512_mask_store_ps(dst_p + (n - n % 16), UINT16_MAX >> (16 - n % 16), _mm512_cvtepi32_ps(x));
	}
}

void word_to_float_avx512f(const void *src, void *dst, size_t n)
{
	const uint16_t *src_p = static_cast<const uint16_t *>(src);
	float *dst_p = static_cast<float *>(dst);

	for (size_t i = 0; i < n - n % 16; i += 16) {
		__m512i x = _mm512_cvtepu16_epi32(_mm256_load_si256((const __m256i *)(src_p + i)));
		_mm512_store_ps(dst_p + i, _mm512_cvtepi32_ps(x));
	}
	if (n % 16) {
		__m512i x = _mm512_cvtepu16_epi32(_mm256_load_si256((const __m256i *)(src_p + (n - n % 16))));
		_mm512_mask_store_ps(dst_p + (n - n % 16), UINT16_MAX >> (16 - n % 16), _mm512_cvtepi32_ps(x));
	}
}

void half_to_float_avx512f(const void *src, void *dst, size_t n)
{
	const uint16_t *src_p = static_cast<const uint16_t *>(src);
	float *dst_p = static_cast<float *>(dst);

	for (size_t i = 0; i < n - n % 16; i += 16) {
		__m512 x = _mm512_cvtph_ps(_mm256_load_si256((const __m256i *)(src_p + i)));
		_mm512_store_ps(dst_p + i, x);
	}
	if (n % 16) {
		__m512 x = _mm512_cvtph_ps(_mm256_load_si256((const __m256i *)(src_p + (n - n % 16))));
		_mm512_mask_store_ps(dst_p + (n - n % 16), UINT16_MAX >> (16 - n % 16), x);
	}
}

void float_to_byte_avx512f(const void *src, void *dst, size_t n)
{
	const float *src_p = static_cast<const float *>(src);
	uint8_t *dst_p = static_cast<uint8_t *>(dst);

	for (size_t i = 0; i < n - n % 16; i += 16) {
		__m512i x = _mm512_cvtps_epi32(_mm512_load_ps(src_p + i));
		x = _mm512_max_epi32(x, _mm512_setzero_si512());
		_mm_store_si128((__m128i *)(dst_p + i), _mm512_cvtusepi32_epi8(x));
	}
	if (n % 16) {
		// 8-bit mask granularity requires AVX-512 BW.
		alignas(16) uint8_t tmp[16];
		__m512i x = _mm512_cvtps_epu32(_mm512_load_ps(src_p + (n - n % 16)));
		x = _mm512_max_epi32(x, _mm512_setzero_si512());
		_mm_store_si128((__m128i *)tmp, _mm512_cvtusepi32_epi8(x));

		for (size_t i = n - n % 16; i < n; ++i) {
			dst_p[i] = tmp[i % 16];
		}
	}
}

void float_to_word_avx512f(const void *src, void *dst, size_t n)
{
	const float *src_p = static_cast<const float *>(src);
	uint16_t *dst_p = static_cast<uint16_t *>(dst);

	for (size_t i = 0; i < n - n % 16; i += 16) {
		__m512i x = _mm512_cvtps_epu32(_mm512_load_ps(src_p + i));
		x = _mm512_max_epi32(x, _mm512_setzero_si512());
		_mm256_store_si256((__m256i *)(dst_p + i), _mm512_cvtusepi32_epi16(x));
	}
	if (n % 16) {
		// 16-bit mask granularity requires AVX-512 BW.
		alignas(32) uint16_t tmp[16];
		__m512i x = _mm512_cvtps_epu32(_mm512_load_ps(src_p + (n - n % 16)));
		x = _mm512_max_epi32(x, _mm512_setzero_si512());
		_mm256_store_si256((__m256i *)tmp, _mm512_cvtusepi32_epi16(x));

		for (size_t i = n - n % 16; i < n; ++i) {
			dst_p[i] = tmp[i % 16];
		}
	}
}

void float_to_half_avx512f(const void *src, void *dst, size_t n)
{
	const float *src_p = static_cast<const float *>(src);
	uint16_t *dst_p = static_cast<uint16_t *>(dst);

	for (size_t i = 0; i < n - n % 16; i += 16) {
		__m256i x = _mm512_cvtps_ph(_mm512_load_ps(src_p + i), 0);
		_mm256_store_si256((__m256i *)(dst_p + i), x);
	}
	if (n % 16) {
		// 16-bit mask granularity requires AVX-512 BW.
		alignas(32) uint16_t tmp[16];
		__m256i x = _mm512_cvtps_ph(_mm512_load_ps(src_p + (n - n % 16)), 0);
		_mm256_store_si256((__m256i *)tmp, x);

		for (size_t i = n - n % 16; i < n; ++i) {
			dst_p[i] = tmp[i % 16];
		}
	}
}


std::unique_ptr<Prescreener> create_prescreener_old_avx512f(const PrescreenerOldCoefficients &coeffs, double pixel_half)
{
	return std::make_unique<PrescreenerOldAVX512F>(coeffs, pixel_half);
}

std::unique_ptr<Prescreener> create_prescreener_new_avx512f(const PrescreenerNewCoefficients &coeffs, double pixel_half)
{
	return std::make_unique<PrescreenerNewAVX512F>(coeffs, pixel_half);
}

std::unique_ptr<Predictor> create_predictor_avx512f(const PredictorModel &model, bool use_q2)
{
	if (model.first.nns >= 32)
		return std::make_unique<PredictorAVX512F<64>>(model, use_q2);
	else
		return std::make_unique<PredictorAVX512F<32>>(model, use_q2);
}

} // namespace znedi3

#endif // ZNEDI3_X86_AVX512
