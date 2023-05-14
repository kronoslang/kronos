#include "config/system.h"

#ifdef WIN32
#define EXPORT_SYMBOL __declspec(dllexport)
#else
#define EXPORT_SYMBOL
#endif


#ifdef WAVECORE_EIGEN_INCLUDE_DIR
#include <Eigen/SVD>

template <class MTX> struct pinverter : public Eigen::JacobiSVD<MTX> {
	pinverter(const MTX& matrix) :JacobiSVD(matrix, Eigen::ComputeFullU | Eigen::ComputeFullV) {}
	template <typename M> void pinv(M& pinvmat) const {
		eigen_assert(m_isInitialized && "SVD is not initialized.");
		double  pinvtoler = 1.e-6; // choose your tolerance wisely!
		SingularValuesType singularValues_inv = m_singularValues;
		for (long i = 0; i<m_workMatrix.cols(); ++i) {
			if (m_singularValues(i) > pinvtoler)
				singularValues_inv(i) = 1 / m_singularValues(i);
			else singularValues_inv(i) = 0;
		}
		pinvmat = (m_matrixV*singularValues_inv.asDiagonal()*m_matrixU.transpose());
	}
};

#include <iostream>

extern "C" EXPORT_SYMBOL  int eigen_pinv(float *pinv, float *matrix, int rows, int cols) {
	__debugbreak();
	using namespace Eigen;
	Eigen::Map<Eigen::Matrix<float,Dynamic,Dynamic,RowMajor>> map(matrix, rows, cols);
	Eigen::Map<Eigen::Matrix<float,Dynamic,Dynamic,RowMajor>> outmap(pinv, cols, rows);
	pinverter<Eigen::MatrixXf> compute_pinv(map);
	compute_pinv.pinv(outmap);

	std::cout << map << "\n" << outmap;
	return 0;
}

#include <iomanip>

extern "C" EXPORT_SYMBOL int eigen_ls_solve(float *solution, float *a, float *b, int mr, int mc) {
	using namespace Eigen;
	MatrixXd in_m(mc, mr);
	VectorXd in_v(mc);

	// transpose and upgrade to double
	for (int r(0);r < mr;++r) {
		for (int c(0);c < mc;++c) {
			in_m(c, r) = a[r * mc + c];
		}
	}

	for (int i(0);i < mc;++i) {
		in_v[i] = b[i];
	}

	auto s = in_m.jacobiSvd(ComputeThinU | ComputeThinV).solve(in_v).eval();
	for (int i(0);i < mr;++i) {
		solution[i] = s[i];
		std::cout << solution[i] << " ";
	}
	std::cout << "\nResponse\n";

	auto v = (in_m * s).eval();

	for (int i(0);i < v.size() / 2;++i) {
		std::cout << sqrtf(v[i] * v[i] + v[i + v.size() / 2] * v[i + v.size() / 2]) << " ";
	}
	std::cout << "\n";

	return 1;
}

#endif

#include "common/kiss_fftr.h"
#include "common/kiss_fft.h"

#include <vector>


extern "C" EXPORT_SYMBOL  int kissfft_hilbert_transform(float *out_cpx, const float *in_real, int numsmp) {
	auto st(kiss_fftr_alloc(numsmp * 2, 0, nullptr, nullptr));
	auto stc(kiss_fft_alloc(numsmp * 2, 1, nullptr, nullptr));

	std::vector<float> pad(in_real, in_real + numsmp);
	pad.resize(numsmp * 2);
	std::vector<kiss_fft_cpx> transform(numsmp + 1);

	kiss_fftr(st, pad.data(), transform.data());

	transform.resize(numsmp * 2);
	for (int i = 1;i < numsmp; ++i) {
		float re = transform[i].r, im = transform[i].i;
		transform[numsmp * 2 - i].i = re;
		transform[numsmp * 2 - i].r = -im;
		transform[i].r = -re;
		transform[i].i = im;
	}
	kiss_fft(stc, transform.data(), transform.data());
	const float norm = 1.f / float(numsmp);
	for (int i(0);i < numsmp;++i) {
		out_cpx[i * 2] = transform[i].r * norm;
		out_cpx[i * 2 + 1] = transform[i].i * norm;
	}
	return 0;
}

extern "C" EXPORT_SYMBOL int kissfft(float *data, int numbins, int dir) {
	auto st(kiss_fft_alloc(numbins, dir, nullptr, nullptr));
	kiss_fft(st, (kiss_fft_cpx*)data, (kiss_fft_cpx*)data);
	kiss_fft_free(st);
	return 1;
}
