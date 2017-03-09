#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include <quda_internal.h>
#include <color_spinor_field.h>
#include <blas_quda.h>
#include <dslash_quda.h>
#include <invert_quda.h>
#include <util_quda.h>
#include <sys/time.h>
#include <string.h>

#include <face_quda.h>

#include <iostream>

#include <blas_magma.h>
#include <algorithm>

#include <Eigen/Dense>
#include <Eigen/Eigenvalues>

/*
GMRES-DR algorithm:
R. B. Morgan, "GMRES with deflated restarting", SIAM J. Sci. Comput. 24 (2002) p. 20-37
See also: A.Frommer et al, "Deflation and Flexible SAP-Preconditioning of GMRES in Lattice QCD simulations" ArXiv hep-lat/1204.5463
*/

namespace quda {

    using namespace blas;

    using namespace Eigen;
    using namespace std;

    using DynamicStride   = Stride<Dynamic, Dynamic>;

    using DenseMatrix     = MatrixXcd;
    using VectorSet       = MatrixXcd;
    using Vector          = VectorXcd;

    struct SortedEvals{

      double _val;
      int    _idx;

      SortedEvals(double val, int idx) : _val(val), _idx(idx) {};
      static bool SelectSmall (SortedEvals v1, SortedEvals v2) { return (v1._val < v2._val);}
    };

    enum class libtype {eigen_lib, magma_lib, lapack_lib, mkl_lib};

    class GMResDRArgs{

      public:

       VectorSet   ritzVecs;
       DenseMatrix H;
       Vector      eta;

       int m;
       int k;
       int restarts;

       Complex      *c;

       GMResDRArgs(int m, int nev) : ritzVecs(VectorSet::Zero(m+1,nev+1)), H(DenseMatrix::Zero(m+1,m)),
       eta(Vector::Zero(m)), m(m), k(nev), restarts(0) { c = static_cast<Complex*> (ritzVecs.col(k).data()); }

       inline void ResetArgs() {
         ritzVecs.setZero();
         H.setZero();
         eta.setZero();
       }

       ~GMResDRArgs(){ }
   };

   template<libtype which_lib> void ComputeHarmonicRitz(GMResDRArgs &args) {errorQuda("\nUnknown library type.\n");}

   template <> void ComputeHarmonicRitz<libtype::magma_lib>(GMResDRArgs &args)
   {
     DenseMatrix cH = args.H.block(0, 0, args.m, args.m).adjoint();
     DenseMatrix Gk = args.H.block(0, 0, args.m, args.m);

     VectorSet  harVecs = MatrixXcd::Zero(args.m, args.m);
     Vector     harVals = VectorXcd::Zero(args.m);

     Vector em = VectorXcd::Zero(args.m);

     em(args.m-1) = norm( args.H(args.m, args.m-1) );

     cudaHostRegister(static_cast<void *>(cH.data()), args.m*args.m*sizeof(Complex), cudaHostRegisterDefault);
     magma_Xgesv(static_cast<void*>(em.data()), args.m, args.m, static_cast<void*>(cH.data()), args.m, sizeof(Complex));
     cudaHostUnregister(cH.data());

     Gk.col(args.m-1) += em;

     cudaHostRegister(static_cast<void *>(Gk.data()), args.m*args.m*sizeof(Complex), cudaHostRegisterDefault);
     magma_Xgeev(static_cast<void*>(Gk.data()), args.m, args.m, static_cast<void*>(harVecs.data()), static_cast<void*>(harVals.data()), args.m, sizeof(Complex));
     cudaHostUnregister(Gk.data());

     std::vector<SortedEvals> sorted_evals;
     sorted_evals.reserve(args.m);

     for(int e = 0; e < args.m; e++) sorted_evals.push_back( SortedEvals( abs(harVals.data()[e]), e ));
     std::stable_sort(sorted_evals.begin(), sorted_evals.end(), SortedEvals::SelectSmall);

     for(int e = 0; e < args.k; e++) memcpy(args.ritzVecs.col(e).data(), harVecs.col(sorted_evals[e]._idx).data(), (args.m)*sizeof(Complex));

     return;
   }


   template <> void ComputeHarmonicRitz<libtype::eigen_lib>(GMResDRArgs &args)
   {
     DenseMatrix cH = args.H.block(0, 0, args.m, args.m).adjoint();
     DenseMatrix Gk = args.H.block(0, 0, args.m, args.m);

     VectorSet  harVecs = MatrixXcd::Zero(args.m, args.m);
     Vector     harVals = VectorXcd::Zero(args.m);

     Vector em = VectorXcd::Zero(args.m);

     em(args.m-1) = norm( args.H(args.m, args.m-1) );
     Gk.col(args.m-1) += cH.colPivHouseholderQr().solve(em);

     ComplexEigenSolver<DenseMatrix> es( Gk );
     harVecs = es.eigenvectors();
     harVals = es.eigenvalues ();     

     std::vector<SortedEvals> sorted_evals;
     sorted_evals.reserve(args.m);

     for(int e = 0; e < args.m; e++) sorted_evals.push_back( SortedEvals( abs(harVals.data()[e]), e ));
     std::stable_sort(sorted_evals.begin(), sorted_evals.end(), SortedEvals::SelectSmall);

     for(int e = 0; e < args.k; e++) memcpy(args.ritzVecs.col(e).data(), harVecs.col(sorted_evals[e]._idx).data(), (args.m)*sizeof(Complex));

     return;
   }


    template<libtype which_lib> void ComputeEta(GMResDRArgs &args) {errorQuda("\nUnknown library type.\n");}

    template <> void ComputeEta<libtype::magma_lib>(GMResDRArgs &args) {

       DenseMatrix Htemp(DenseMatrix::Zero(args.m+1,args.m));
       Htemp = args.H; 

       Complex *ctemp = static_cast<Complex*> (args.ritzVecs.col(0).data());
       memcpy(ctemp, args.c, (args.m+1)*sizeof(Complex));

       cudaHostRegister(static_cast<void*>(Htemp.data()), (args.m+1)*args.m*sizeof(Complex), cudaHostRegisterDefault);
       magma_Xgels(static_cast<void*>(Htemp.data()), ctemp, args.m+1, args.m, args.m+1, sizeof(Complex));
       cudaHostUnregister(Htemp.data());

       memcpy(args.eta.data(), ctemp, args.m*sizeof(Complex));
       memset(ctemp, 0, (args.m+1)*sizeof(Complex));

       return;
    }

    template <> void ComputeEta<libtype::eigen_lib>(GMResDRArgs &args) {

        Map<VectorXcd, Unaligned> c_(args.c, args.m+1);
        args.eta = args.H.jacobiSvd(ComputeThinU | ComputeThinV).solve(c_);

       return;
    }


    void fillInnerSolveParam_(SolverParam &inner, const SolverParam &outer) {
      inner.tol = outer.tol_precondition;
      inner.maxiter = outer.maxiter_precondition;
      inner.delta = 1e-20; 
      inner.inv_type = outer.inv_type_precondition;

      inner.precision = outer.precision_precondition; 
      inner.precision_sloppy = outer.precision_precondition;

      inner.iter = 0;
      inner.gflops = 0;
      inner.secs = 0;

      inner.inv_type_precondition = QUDA_INVALID_INVERTER;
      inner.is_preconditioner = true; 

      inner.global_reduction = false;

      if (outer.precision_sloppy != outer.precision_precondition)
        inner.preserve_source = QUDA_PRESERVE_SOURCE_NO;
      else inner.preserve_source = QUDA_PRESERVE_SOURCE_YES;
    }


 GMResDR::GMResDR(DiracMatrix &mat, DiracMatrix &matSloppy, DiracMatrix &matPrecon, SolverParam &param, TimeProfile &profile) :
    Solver(param, profile), mat(mat), matSloppy(matSloppy), matPrecon(matPrecon), K(nullptr), Kparam(param),
    Vm(nullptr), Zm(nullptr), profile(profile), gmresdr_args(nullptr), init(false)
 {
     fillInnerSolveParam_(Kparam, param);

     if (param.inv_type_precondition == QUDA_CG_INVERTER) 
       K = new CG(matPrecon, matPrecon, Kparam, profile);
     else if (param.inv_type_precondition == QUDA_BICGSTAB_INVERTER) 
       K = new BiCGstab(matPrecon, matPrecon, matPrecon, Kparam, profile);
     else if (param.inv_type_precondition == QUDA_MR_INVERTER) 
       K = new MR(matPrecon, matPrecon, Kparam, profile);
     else if (param.inv_type_precondition == QUDA_SD_INVERTER) 
       K = new SD(matPrecon, Kparam, profile);
     else if (param.inv_type_precondition == QUDA_INVALID_INVERTER) 
       K = nullptr;
     else
       errorQuda("Unsupported preconditioner %d\n", param.inv_type_precondition);

     return;
 }

 GMResDR::GMResDR(DiracMatrix &mat, Solver &K, DiracMatrix &matSloppy, DiracMatrix &matPrecon, SolverParam &param, TimeProfile &profile) :
    Solver(param, profile), mat(mat), matSloppy(matSloppy), matPrecon(matPrecon), K(&K), Kparam(param),
    Vm(nullptr), Zm(nullptr), profile(profile), gmresdr_args(nullptr), init(false) { }


 GMResDR::~GMResDR() {
    profile.TPSTART(QUDA_PROFILE_FREE);

    if(init)
    {
      delete Vm;
      Vm = nullptr;

      if(K) delete Zm;

      if (param.precision_sloppy != param.precision)  delete r_sloppy;

      if(K && (param.precision_precondition != param.precision_sloppy))
      {
        delete r_pre;
        delete p_pre;
      }

      delete tmpp;
      delete yp;
      delete rp;

      delete gmresdr_args;
    }

   profile.TPSTOP(QUDA_PROFILE_FREE);
 }

#define EIGEN_GELS
 void GMResDR::UpdateSolution(ColorSpinorField *x, ColorSpinorField *r, bool do_gels)
 {
   GMResDRArgs &args = *gmresdr_args;

   if(do_gels) {
#ifdef EIGEN_GELS
     ComputeEta<libtype::eigen_lib>(args);
#else
     ComputeEta<libtype::magma_lib>(args);
#endif
   }

   std::vector<ColorSpinorField*> Z_(Zm->Components().begin(),Zm->Components().begin()+args.m);
   std::vector<ColorSpinorField*> V_(Vm->Components());

   std::vector<ColorSpinorField*> x_, r_;
   x_.push_back(x), r_.push_back(r);

   blas::caxpy( static_cast<Complex*> ( args.eta.data()), Z_, x_);

   VectorXcd minusHeta = - (args.H * args.eta);
   Map<VectorXcd, Unaligned> c_(args.c, args.m+1);
   c_ += minusHeta;

   blas::caxpy(static_cast<Complex*>(minusHeta.data()), V_, r_);

   return;
 }

//#define USE_MAGMA

 void GMResDR::RestartVZH()
 {
   GMResDRArgs &args = *gmresdr_args;
#ifdef USE_MAGMA
   ComputeHarmonicRitz<libtype::magma_lib>(args);
#else
   ComputeHarmonicRitz<libtype::eigen_lib>(args);
#endif

   DenseMatrix Qkp1(MatrixXcd::Identity((args.m+1), (args.k+1)));

   HouseholderQR<MatrixXcd> qr(args.ritzVecs);
   Qkp1.applyOnTheLeft( qr.householderQ());

   DenseMatrix Res = Qkp1.adjoint()*args.H*Qkp1.topLeftCorner(args.m, args.k);
   args.H.setZero();
   args.H.topLeftCorner(args.k+1, args.k) = Res;

   ColorSpinorParam csParam(Vm->Component(0));

   csParam.is_composite  = true;
   csParam.composite_dim = (args.k+1);
   csParam.create = QUDA_ZERO_FIELD_CREATE;
   csParam.setPrecision(QUDA_DOUBLE_PRECISION);

   ColorSpinorFieldSet *Vkp1 = ColorSpinorFieldSet::Create(csParam);

   std::vector<ColorSpinorField*> V(Vm->Components());

   for(int i = 0; i < (args.k+1); i++)
   {
     std::vector<ColorSpinorField*> Vi(Vkp1->Components().begin()+i,Vkp1->Components().begin()+i+1);
     blas::caxpy(static_cast<Complex*>(Qkp1.col(i).data()), V , Vi);
   }

   for(int i = 0; i < (args.m+1); i++)
   {
     if(i < (args.k+1) )
     {
       blas::copy(Vm->Component(i), Vkp1->Component(i));
       blas::zero(Vkp1->Component(i));
     }
     else blas::zero(Vm->Component(i));
   }

   if( Zm->V() != Vm->V() )
   {
     DenseMatrix Qk = Qkp1.topLeftCorner(args.m,args.k);

     std::vector<ColorSpinorField*> Z(Zm->Components());

     for(int i = 0; i < args.k; i++)
     {
       std::vector<ColorSpinorField*> Vi(Vkp1->Components().begin()+i,Vkp1->Components().begin()+i+1);
       blas::caxpy(static_cast<Complex*>(Qkp1.col(i).data()), Z , Vi);
     }

     for(int i = 0; i < (args.m); i++)
     {
       if( i < (args.k) ) blas::copy(Zm->Component(i), Vkp1->Component(i));
       else               blas::zero(Zm->Component(i));
     }
   }

   delete Vkp1;

   checkCudaError();

   for(int j = 0; j < args.k; j++)
   {
     Complex alpha = cDotProduct(Vm->Component(j), Vm->Component(args.k));
     caxpy(-alpha, Vm->Component(j), Vm->Component(args.k));
   }

   blas::ax(1.0/ sqrt(blas::norm2(Vm->Component(args.k))), Vm->Component(args.k));

   args.ritzVecs.setZero();

   return;
 }


int GMResDR::FlexArnoldiProcedure(const int start_idx, const bool do_givens = false)
 {
   GMResDRArgs &args = *gmresdr_args;
   ColorSpinorField &tmp = *tmpp;

   Complex *givensH = (do_givens) ? new Complex[(args.m+1)*args.m] : nullptr;
   Complex *cn      = (do_givens) ? new Complex[args.m]            : nullptr;
   double  *sn      = (do_givens) ? new double [args.m]            : nullptr;

   Complex c0 = args.c[0];

   int j = start_idx;

   while( j < args.m ) 
   {
     if(K) {
       ColorSpinorField &inPre  = (param.precision_precondition != param.precision_sloppy) ? *r_pre : Vm->Component(j);
       ColorSpinorField &outPre = (param.precision_precondition != param.precision_sloppy) ? *p_pre : Zm->Component(j);

       if(param.precision_precondition != param.precision_sloppy) inPre = Vm->Component(j);
       zero(outPre);
       (*K)( outPre ,inPre );

       if(param.precision_precondition != param.precision_sloppy) Zm->Component(j) = outPre;
     }
     matSloppy(Vm->Component(j+1), Zm->Component(j), tmp);

     args.H(0, j) = cDotProduct(Vm->Component(0), Vm->Component(j+1));
     caxpy(-args.H(0, j), Vm->Component(0), Vm->Component(j+1));

     Complex h0 = do_givens ? args.H(0, j) : 0.0;

     for(int i = 1; i <= j; i++)
     {
        args.H(i, j) = cDotProduct(Vm->Component(i), Vm->Component(j+1));
        caxpy(-args.H(i, j), Vm->Component(i), Vm->Component(j+1));

        if(do_givens) {
           givensH[(args.m+1)*j+(i-1)] = conj(cn[i-1])*h0 + sn[i-1]*args.H(i,j);
           h0 = -sn[i-1]*h0 + cn[i-1]*args.H(i,j);
        }
     }

     args.H(j+1, j) = Complex(sqrt(norm2(Vm->Component(j+1))), 0.0);
     blas::ax( 1.0 / args.H(j+1, j).real(), Vm->Component(j+1));
     if(do_givens)
     {
       double inv_denom = 1.0 / sqrt(norm(h0)+norm(args.H(j+1,j)));
       cn[j] = h0 * inv_denom;
       sn[j] = args.H(j+1,j).real() * inv_denom;
       givensH[j*(args.m+1)+j] = conj(cn[j])*h0 + sn[j]*args.H(j+1,j);

       args.c[j+1] = -sn[j]*args.c[j];
       args.c[j]  *= conj(cn[j]);
     }

     j += 1;
   }

   if(do_givens)
   {
     Map<MatrixXcd, Unaligned, DynamicStride> givensH_(givensH, args.m, args.m, DynamicStride(args.m+1,1) );
     memcpy(args.eta.data(),  args.c, args.m*sizeof(Complex));
     memset(args.c, 0, (args.m+1)*sizeof(Complex));
     args.c[0] = c0;

     givensH_.triangularView<Upper>().solveInPlace<OnTheLeft>(args.eta);

     delete[] givensH;
     delete[] cn;
     delete[] sn;

   } else {
     const int cdot_pipeline_length  = 5;
     int offset = 0;
     memset(args.c, 0, (args.m+1)*sizeof(Complex));

     do {
        const int local_length = ((args.k+1) - offset) > cdot_pipeline_length  ? cdot_pipeline_length : ((args.k+1) - offset) ;

        std::vector<cudaColorSpinorField*> v_;
        std::vector<cudaColorSpinorField*> r_;
        v_.reserve(local_length);
        r_.reserve(local_length);

        for(int i = 0; i < local_length; i++)
        {
          v_.push_back(static_cast<cudaColorSpinorField*>(&Vm->Component(offset+i)));
          r_.push_back(static_cast<cudaColorSpinorField*>(r_sloppy));
        }
        blas::cDotProduct(&args.c[offset], v_, r_);
        offset += cdot_pipeline_length;

     } while (offset < (args.k+1));
   }

   return (j-start_idx);
 }

 void GMResDR::operator()(ColorSpinorField &x, ColorSpinorField &b)
 {
    profile.TPSTART(QUDA_PROFILE_INIT);

    const double tol_threshold     = 1.2;
    const double det_max_deviation = 0.8;

    if (!init) {

      gmresdr_args = new GMResDRArgs(param.m, param.nev);

      ColorSpinorParam csParam(b);

      yp = ColorSpinorField::Create(b); 
      rp = ColorSpinorField::Create(b); 

      csParam.create = QUDA_ZERO_FIELD_CREATE;
      csParam.setPrecision(param.precision_sloppy);

      tmpp     = ColorSpinorField::Create(csParam);
      r_sloppy = (param.precision_sloppy != param.precision) ? ColorSpinorField::Create(csParam) : rp;

      if ( K && (param.precision_precondition != param.precision_sloppy) ) {

        csParam.setPrecision(param.precision_precondition);
        p_pre = ColorSpinorField::Create(csParam);
        r_pre = ColorSpinorField::Create(csParam);

      }

      csParam.setPrecision(param.precision_sloppy);
      csParam.is_composite  = true;
      csParam.composite_dim = param.m+1;

      Vm   = ColorSpinorFieldSet::Create(csParam); 

      csParam.composite_dim = param.m;

      Zm = K ? ColorSpinorFieldSet::Create(csParam) : Vm;

      init = true;
    }

    GMResDRArgs &args = *gmresdr_args;

    ColorSpinorField &r   = *rp;
    ColorSpinorField &y   = *yp;
    ColorSpinorField &rSloppy = *r_sloppy;

    profile.TPSTOP(QUDA_PROFILE_INIT);
    profile.TPSTART(QUDA_PROFILE_PREAMBLE);

    int tot_iters = 0;

    double normb = norm2( b );
    double stop  = param.tol*param.tol* normb;  

    mat(r, x, y);
    
    double r2 = xmyNorm(b, r);
    double b2     = r2;

    args.c[0] = Complex(sqrt(r2), 0.0);

    printfQuda("\nInitial residual squared: %1.16e, source %1.16e, tolerance %1.16e\n", r2, sqrt(normb), param.tol);


    if(param.precision_sloppy != param.precision) {
      blas::copy(rSloppy, r);

      blas::ax(1.0 / args.c[0].real(), r);   
      Vm->Component(0) = r;
    } else {
      blas::zero(Vm->Component(0));
      blas::axpy(1.0 / args.c[0].real(), rSloppy, Vm->Component(0));   
    }

    profile.TPSTOP(QUDA_PROFILE_PREAMBLE);
    profile.TPSTART(QUDA_PROFILE_COMPUTE);
    blas::flops = 0;

    const bool use_heavy_quark_res = (param.residual_type & QUDA_HEAVY_QUARK_RESIDUAL) ? true : false;

    double heavy_quark_res = 0.0;  
    if (use_heavy_quark_res)  heavy_quark_res = sqrt(blas::HeavyQuarkResidualNorm(x, r).z);


   int restart_idx = 0, j = 0, check_interval = 4;

   DenseMatrix Gm = DenseMatrix::Zero(args.k+1, args.k+1);

   while(restart_idx < param.deflation_grid && !(convergence(r2, heavy_quark_res, stop, param.tol_hq) || !(r2 > stop)))
   {
     tot_iters += FlexArnoldiProcedure(j, (j == 0));
     UpdateSolution(&x, r_sloppy, !(j == 0));

     r2 = norm2(rSloppy);

     bool   do_clean_restart = false;
     double ext_r2 = 1.0;

     if((restart_idx+1) % check_interval) {
       mat(r, x, y);
       ext_r2 = xmyNorm(b, r);

       for(int l = 0; l < args.k+1; l++) {

         const int cdot_pipeline_length  = 1;
         int offset = 0;

         Complex *col = Gm.col(l).data();

         do {
           const int local_length = ((args.k+1) - offset) > cdot_pipeline_length  ? cdot_pipeline_length : ((args.k+1) - offset) ;

            std::vector<cudaColorSpinorField*> v1_;
            std::vector<cudaColorSpinorField*> v2_;
            v1_.reserve(local_length);
            v2_.reserve(local_length);

            for(int i = 0; i < local_length; i++)
            {
              v1_.push_back(static_cast<cudaColorSpinorField*>(&Vm->Component(offset+i)));
              v2_.push_back(static_cast<cudaColorSpinorField*>(&Vm->Component(l)));
            }
            blas::cDotProduct(&col[offset], v1_, v2_);

            offset += cdot_pipeline_length;

         } while (offset < (args.k+1));
       }//end l-loop

       Complex detGm = Gm.determinant();

       PrintStats("FGMResDR:", tot_iters, r2, b2, heavy_quark_res);
       printfQuda("\nCheck cycle %d, true residual squared %1.15e, Gramm det : (%le, %le)\n", restart_idx, ext_r2, detGm.real(), detGm.imag());

       Gm.setZero();

       do_clean_restart = ((sqrt(ext_r2) / sqrt(r2)) > tol_threshold) || (norm(detGm) > det_max_deviation);
     }

     if( ((restart_idx != param.deflation_grid-1) && !do_clean_restart) ) {

       RestartVZH();
       j = args.k;

     } else {

       printfQuda("\nClean restart for cycle %d, true residual squared %1.15e\n", restart_idx, ext_r2);

       args.ResetArgs();
       memset(args.c, 0, (args.m+1) * sizeof(Complex));
       args.c[0] = Complex(sqrt(ext_r2), 0.0);
       blas::zero(Vm->Component(0));
       blas::axpy(1.0 / args.c[0].real(), rSloppy, Vm->Component(0));

       j = 0;
     }

     restart_idx += 1;

   }

   profile.TPSTOP(QUDA_PROFILE_COMPUTE);
   profile.TPSTART(QUDA_PROFILE_EPILOGUE);

   param.secs = profile.Last(QUDA_PROFILE_COMPUTE);
   double gflops = (blas::flops + mat.flops())*1e-9;
   param.gflops = gflops;
   param.iter += tot_iters;

   mat(r, x, y);

   param.true_res = sqrt(xmyNorm(b, r) / b2);

   PrintSummary("FGMResDR:", tot_iters, r2, b2);

   blas::flops = 0;
   mat.flops();

   profile.TPSTOP(QUDA_PROFILE_EPILOGUE);

   param.rhs_idx += 1;

   return;
 }

} // namespace quda
