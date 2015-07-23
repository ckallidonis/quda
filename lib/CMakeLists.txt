
#temporary workaround
set (COMM_OBJS comm_single.cpp)

set (QUDA_OBJS
  gauge_phase.cu timer.cpp malloc.cpp solver.cpp
	inv_bicgstab_quda.cpp inv_cg_quda.cpp inv_multi_cg_quda.cpp
	inv_eigcg_quda.cpp gauge_ape.cu gauge_plaq.cu inv_gcr_quda.cpp
	inv_mr_quda.cpp inv_sd_quda.cpp inv_xsd_quda.cpp inv_pcg_quda.cpp
	inv_mre.cpp interface_quda.cpp util_quda.cpp color_spinor_field.cpp
	color_spinor_util.cu cpu_color_spinor_field.cpp
	cuda_color_spinor_field.cu dirac.cpp hw_quda.cpp blas_cpu.cpp
	clover_field.cpp lattice_field.cpp gauge_field.cpp
	cpu_gauge_field.cpp cuda_gauge_field.cu extract_gauge_ghost.cu
	max_gauge.cu gauge_update_quda.cu dirac_clover.cpp dirac_wilson.cpp
	dirac_staggered.cpp dirac_improved_staggered.cpp
	dirac_domain_wall.cpp dirac_domain_wall_4d.cpp dirac_mobius.cpp
	dirac_twisted_clover.cpp dirac_twisted_mass.cpp tune.cpp
	fat_force_quda.cpp llfat_quda_itf.cpp llfat_quda.cu
	gauge_force_quda.cu clover_quda.cu dslash_quda.cu covDev.cu
	dslash_wilson.cu dslash_clover.cu dslash_clover_asym.cu
	dslash_twisted_mass.cu dslash_ndeg_twisted_mass.cu
	dslash_twisted_clover.cu dslash_domain_wall.cu
	dslash_domain_wall_4d.cu dslash_mobius.cu dslash_staggered.cu
	dslash_improved_staggered.cu dslash_pack.cu blas_quda.cu
	copy_quda.cu reduce_quda.cu face_buffer.cpp face_gauge.cpp
	comm_common.cpp ${COMM_OBJS} ${NUMA_AFFINITY_OBJS}
	clover_deriv_quda.cu clover_invert.cu copy_gauge_extended.cu
	extract_gauge_ghost_extended.cu copy_color_spinor.cu
	copy_gauge_double.cu copy_gauge_single.cu copy_gauge_half.cu
	copy_gauge.cu copy_clover.cu staggered_oprod.cu
	clover_trace_quda.cu ks_force_quda.cu hisq_paths_force_quda.cu
	fermion_force_quda.cu unitarize_force_quda.cu
	unitarize_links_quda.cu milc_interface.cpp
	extended_color_spinor_utilities.cu eig_lanczos_quda.cpp
	ritz_quda.cpp eig_solver.cpp blas_magma.cpp misc_helpers.cu
	inv_mpcg_quda.cpp inv_mpbicgstab_quda.cpp
)

include_directories(dslash_core)
include_directories(.)
cuda_add_library(quda STATIC ${QUDA_OBJS})