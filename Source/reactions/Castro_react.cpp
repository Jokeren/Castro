
#include <Castro.H>
#include <Castro_F.H>

using std::string;
using namespace amrex;

// Strang version

bool
Castro::react_state(MultiFab& s, MultiFab& r, Real time, Real dt)
{
    BL_PROFILE("Castro::react_state()");

    // Sanity check: should only be in here if we're doing CTU.

    if (time_integration_method != CornerTransportUpwind) {
        amrex::Error("Strang reactions are only supported for the CTU advance.");
    }

    // Sanity check: cannot use CUDA without a network with a C++ implementation.

#if defined(AMREX_USE_GPU) && !defined(NETWORK_HAS_CXX_IMPLEMENTATION)
    static_assert(false, "Cannot compile for GPUs if using a network without a C++ implementation.");
#endif

    const Real strt_time = ParallelDescriptor::second();

    // Start off by assuming a successful burn.

    int burn_success = 1;

    if (do_react != 1) {

        // Ensure we always have valid data, even if we don't do the burn.
        r.setVal(0.0, r.nGrow());

        return burn_success;

    }

    // Check if we have any zones to burn.

    if (!valid_zones_to_burn(s)) {

        // Ensure we always have valid data, even if we don't do the burn.
        r.setVal(0.0, r.nGrow());

        return burn_success;

    }

    // If we're not actually doing the burn, interpolate from the level below.

    if (level > castro::reactions_max_solve_level && level > 0) {
        FillCoarsePatch(r, 0, time, Reactions_Type, 0, r.nComp(), r.nGrow());
    }

    const int ng = s.nGrow();

    if (verbose) {
        amrex::Print() << "... Entering burner and doing half-timestep of burning." << std::endl << std::endl;
    }

    ReduceOps<ReduceOpSum> reduce_op;
    ReduceData<Real> reduce_data(reduce_op);
    using ReduceTuple = typename decltype(reduce_data)::Type;

#ifdef _OPENMP
#pragma omp parallel
#endif
    for (MFIter mfi(s, TilingIfNotGPU()); mfi.isValid(); ++mfi)
    {

        const Box& bx = mfi.growntilebox(ng);

        auto U = s.array(mfi);
        auto reactions = r.array(mfi);

        if (level <= castro::reactions_max_solve_level) {

            reduce_op.eval(bx, reduce_data,
            [=] AMREX_GPU_HOST_DEVICE (int i, int j, int k) -> ReduceTuple
            {

                burn_t burn_state;

                // Initialize some data for later.

                bool do_burn = true;
                burn_state.success = true;
                Real burn_failed = 0.0_rt;

                // Don't burn on zones inside shock regions, if the relevant option is set.

#ifdef SHOCK_VAR
                if (U(i,j,k,USHK) > 0.0_rt && disable_shock_burning == 1) {
                    do_burn = false;
                }
#endif

                Real rhoInv = 1.0_rt / U(i,j,k,URHO);

                burn_state.rho = U(i,j,k,URHO);
                burn_state.T   = U(i,j,k,UTEMP);
                burn_state.e   = 0.0_rt; // Energy generated by the burn

                for (int n = 0; n < NumSpec; ++n) {
                    burn_state.xn[n] = U(i,j,k,UFS+n) * rhoInv;
                }

#if NAUX_NET > 0
                for (int n = 0; n < NumAux; ++n) {
                    burn_state.aux[n] = U(i,j,k,UFX+n) * rhoInv;
                }
#endif

                // Ensure we start with no RHS or Jacobian calls registered.

                burn_state.n_rhs = 0;
                burn_state.n_jac = 0;

                // Don't burn if we're outside of the relevant (rho, T) range.

                if (burn_state.T < castro::react_T_min || burn_state.T > castro::react_T_max ||
                    burn_state.rho < castro::react_rho_min || burn_state.rho > castro::react_rho_max) {
                    do_burn = false;
                }

                if (do_burn) {
                    burner(burn_state, dt);
                }

                // If we were unsuccessful, update the failure count.

                if (!burn_state.success) {
                    burn_failed = 1.0_rt;
                }

                if (do_burn) {

                    // Add burning rates to reactions MultiFab, but be
                    // careful because the reactions and state MFs may
                    // not have the same number of ghost cells.

                    if (reactions.contains(i,j,k)) {
                        for (int n = 0; n < NumSpec; ++n) {
                            reactions(i,j,k,n) = U(i,j,k,URHO) * (burn_state.xn[n] - U(i,j,k,UFS+n) * rhoInv) / dt;
                        }
#if NAUX_NET > 0
                        for (int n = 0; n < NumAux; ++n) {
                            reactions(i,j,k,n+NumSpec) = U(i,j,k,URHO) * (burn_state.aux[n] - U(i,j,k,UFX+n) * rhoInv) / dt;
                        }
#endif
                        reactions(i,j,k,NumSpec+NumAux  ) = U(i,j,k,URHO) * burn_state.e / dt;
                        reactions(i,j,k,NumSpec+NumAux+1) = amrex::max(1.0_rt, static_cast<Real>(burn_state.n_rhs + 2 * burn_state.n_jac));
                    }

                }
                else {

                    if (reactions.contains(i,j,k)) {
                        for (int n = 0; n < NumSpec + NumAux + 1; ++n) {
                            reactions(i,j,k,n) = 0.0_rt;
                        }

                        reactions(i,j,k,NumSpec+NumAux+1) = 1.0_rt;
                    }

                }

                return {burn_failed};

            });

        }

        // Now update the state with the reactions data.

        amrex::ParallelFor(bx,
        [=] AMREX_GPU_HOST_DEVICE (int i, int j, int k)
        {
            if (U.contains(i,j,k) && reactions.contains(i,j,k)) {
                for (int n = 0; n < NumSpec; ++n) {
                    U(i,j,k,UFS+n) += reactions(i,j,k,n) * dt;
                }
#if NAUX_NET > 0
                for (int n = 0; n < NumAux; ++n) {
                    U(i,j,k,UFX+n) += reactions(i,j,k,n+NumSpec) * dt;
                }
#endif
                U(i,j,k,UEINT) += reactions(i,j,k,NumSpec+NumAux) * dt;
                U(i,j,k,UEDEN) += reactions(i,j,k,NumSpec+NumAux) * dt;
            }
        });

    }

    ReduceTuple hv = reduce_data.value();
    Real burn_failed = amrex::get<0>(hv);

    if (burn_failed != 0.0) {
      burn_success = 0;
    }

    ParallelDescriptor::ReduceIntMin(burn_success);

    if (print_update_diagnostics) {

        Real e_added = r.sum(NumSpec + 1);

        if (e_added != 0.0) {
            amrex::Print() << "... (rho e) added from burning: " << e_added << std::endl << std::endl;
        }

    }

    if (verbose) {
        amrex::Print() << "... Leaving burner after completing half-timestep of burning." << std::endl << std::endl;
    }

    if (verbose > 0)
    {
        const int IOProc   = ParallelDescriptor::IOProcessorNumber();
        Real      run_time = ParallelDescriptor::second() - strt_time;

#ifdef BL_LAZY
        Lazy::QueueReduction( [=] () mutable {
#endif
        ParallelDescriptor::ReduceRealMax(run_time,IOProc);

        amrex::Print() << "Castro::react_state() time = " << run_time << "\n" << "\n";
#ifdef BL_LAZY
        });
#endif
    }

    return burn_success;

}

#ifdef SIMPLIFIED_SDC
// Simplified SDC version

bool
Castro::react_state(Real time, Real dt)
{

    // The goal is to update S_old to S_new with the effects of both
    // advection and reactions.  We come into this routine with the
    // -div{F} stored in hydro_source, and the old and new-time
    // sources stored in Source_Type.  Together we create an advective
    // update of the form: -div{F} + 0.5 (old_source + new_source) and
    // pass this to the reaction integrator where it is applied
    // together with the reactions to update the full state.

    // Note: S_new actually is already updated with just advection, so
    // in the event that we do not react on a zone (e.g., because it
    // doesn't meet the thermodynamic thresholds) we don't have to do
    // anything.  If we do react, then we overwrite what is stored in
    // S_new with the combined effects of advection and reactions.

    BL_PROFILE("Castro::react_state()");

    // Sanity check: should only be in here if we're doing simplified SDC.

    if (time_integration_method != SimplifiedSpectralDeferredCorrections) {
        amrex::Error("This react_state interface is only supported for simplified SDC.");
    }

    // Sanity check: cannot use CUDA without a network with a C++ implementation.

#if defined(AMREX_USE_GPU) && !defined(NETWORK_HAS_CXX_IMPLEMENTATION)
    static_assert(false, "Cannot compile for GPUs if using a network without a C++ implementation.");
#endif

    const Real strt_time = ParallelDescriptor::second();

    if (verbose) {
        amrex::Print() << "... Entering burner and doing full timestep of burning." << std::endl << std::endl;
    }

    MultiFab& S_old = get_old_data(State_Type);
    MultiFab& S_new = get_new_data(State_Type);

    const int ng = S_new.nGrow();

    // Create a MultiFab with all of the non-reacting source terms.
    // This is the term A = -div{F} + 0.5 * (old_source + new_source)

    MultiFab A_src(grids, dmap, NUM_STATE, ng);
    sum_of_sources(A_src);

    MultiFab& reactions = get_new_data(Reactions_Type);

#ifdef NSE_THERMO
    // we need access to the reactive sources if we are doing NSE

    MultiFab& SDC_react_new = get_new_data(Simplified_SDC_React_Type);
#endif

    reactions.setVal(0.0, reactions.nGrow());

    // Start off assuming a successful burn.

    int burn_success = 1;

    ReduceOps<ReduceOpSum> reduce_op;
    ReduceData<Real> reduce_data(reduce_op);

    using ReduceTuple = typename decltype(reduce_data)::Type;

    for (MFIter mfi(S_new, TilingIfNotGPU()); mfi.isValid(); ++mfi)
    {

        const Box& bx = mfi.growntilebox(ng);

        auto U_old = S_old.array(mfi);
        auto U_new = S_new.array(mfi);
        auto asrc = A_src.array(mfi);
        auto react_src = reactions.array(mfi);
#ifdef NSE_THERMO
        auto Iq = SDC_react_new.array(mfi);
#endif

        int lsdc_iteration = sdc_iteration;

        reduce_op.eval(bx, reduce_data,
        [=] AMREX_GPU_HOST_DEVICE (int i, int j, int k) -> ReduceTuple
        {

            burn_t burn_state;

            // Initialize some data for later.

            bool do_burn = true;
            burn_state.success = true;
            Real burn_failed = 0.0_rt;

            // Don't burn on zones inside shock regions, if the
            // relevant option is set.

#ifdef SHOCK_VAR
            if (U_new(i,j,k,USHK) > 0.0_rt && disable_shock_burning == 1) {
                do_burn = false;
            }
#endif

            // Feed in the old-time state data.

            burn_state.y[SRHO] = U_old(i,j,k,URHO);
            burn_state.y[SMX] = U_old(i,j,k,UMX);
            burn_state.y[SMY] = U_old(i,j,k,UMY);
            burn_state.y[SMZ] = U_old(i,j,k,UMZ);
            burn_state.y[SEDEN] = U_old(i,j,k,UEDEN);
            burn_state.y[SEINT] = U_old(i,j,k,UEINT);
            for (int n = 0; n < NumSpec; n++) {
                burn_state.y[SFS+n] = U_old(i,j,k,UFS+n);
            }
#if NAUX_NET > 0
            for (int n = 0; n < NumAux; n++) {
                burn_state.y[SFX+n] = U_old(i,j,k,UFX+n);
            }
#endif
#if NSE_THERMO
            // load up the primitive variable reactive source

            for (int n = 0; n < NumAux; n++) {
                burn_state.Iq_aux[n] = Iq(i,j,k,QFX+n);
            }
            burn_state.Iq_rhoe = Iq(i,j,k,QREINT);
#endif
            // we need an initial T guess for the EOS
            burn_state.T = U_old(i,j,k,UTEMP);

            burn_state.rho = burn_state.y[SRHO];

            // Don't burn if we're outside of the relevant (rho, T) range.

            if (U_old(i,j,k,UTEMP) < castro::react_T_min || U_old(i,j,k,UTEMP) > castro::react_T_max ||
                U_old(i,j,k,URHO) < castro::react_rho_min || U_old(i,j,k,URHO) > castro::react_rho_max) {
                do_burn = false;
            }

             // Tell the integrator about the non-reacting source terms.

             burn_state.ydot_a[SRHO] = asrc(i,j,k,URHO);
             burn_state.ydot_a[SMX] = asrc(i,j,k,UMX);
             burn_state.ydot_a[SMY] = asrc(i,j,k,UMY);
             burn_state.ydot_a[SMZ] = asrc(i,j,k,UMZ);
             burn_state.ydot_a[SEDEN] = asrc(i,j,k,UEDEN);
             burn_state.ydot_a[SEINT] = asrc(i,j,k,UEINT);
             for (int n = 0; n < NumSpec; n++) {
                 burn_state.ydot_a[SFS+n] = asrc(i,j,k,UFS+n);
             }
             for (int n = 0; n < NumAux; n++) {
                 burn_state.ydot_a[SFX+n] = asrc(i,j,k,UFX+n);
             }

             // dual energy formalism: in doing EOS calls in the burn,
             // switch between e and (E - K) depending on (E - K) / E.

             burn_state.T_from_eden = false;

             burn_state.i = i;
             burn_state.j = j;
             burn_state.k = k;

             burn_state.sdc_iter = lsdc_iteration;
             burn_state.num_sdc_iters = sdc_iters;

             if (do_burn) {
                 burner(burn_state, dt);
             }

             // If we were unsuccessful, update the failure count.

             if (!burn_state.success) {
                 burn_failed = 1.0_rt;
             }

             if (do_burn) {

                 // update the state data.

                 U_new(i,j,k,UEDEN) = burn_state.y[SEDEN];
                 U_new(i,j,k,UEINT) = burn_state.y[SEINT];
                 for (int n = 0; n < NumSpec; n++) {
                     U_new(i,j,k,UFS+n) = burn_state.y[SFS+n];
                 }
#if NAUX_NET > 0
                 for (int n = 0; n < NumAux; n++) {
                     U_new(i,j,k,UFX+n) = burn_state.y[SFX+n];
                 }
#endif

                 if (react_src.contains(i,j,k)) {
                     for (int n = 0; n < NumSpec; ++n) {
                         react_src(i,j,k,n) = (U_new(i,j,k,UFS+n) - U_old(i,j,k,UFS+n)) / dt;
                     }
#if NAUX_NET > 0
                     for (int n = 0; n < NumAux; ++n) {
                         react_src(i,j,k,n+NumSpec) = (U_new(i,j,k,UFX+n) - U_old(i,j,k,UFX+n)) / dt;
                     }
#endif

                     react_src(i,j,k,NumSpec+NumAux) = (U_new(i,j,k,UEINT) - U_old(i,j,k,UEINT)) / dt;
                     react_src(i,j,k,NumSpec+NumAux+1) = amrex::max(1.0_rt, static_cast<Real>(burn_state.n_rhs + 2 * burn_state.n_jac));
                 }

             }


             return {burn_failed};
        });

    }

    ReduceTuple hv = reduce_data.value();
    Real burn_failed = amrex::get<0>(hv);

    if (burn_failed != 0.0) burn_success = 0;

    ParallelDescriptor::ReduceIntMin(burn_success);

    if (ng > 0) {
        S_new.FillBoundary(geom.periodicity());
    }

    if (print_update_diagnostics) {

        Real e_added = reactions.sum(NumSpec + 1);

        if (e_added != 0.0)
            amrex::Print() << "... (rho e) added from burning: " << e_added << std::endl << std::endl;

    }

    if (verbose) {

        amrex::Print() << "... Leaving burner after completing full timestep of burning." << std::endl << std::endl;

        const int IOProc   = ParallelDescriptor::IOProcessorNumber();
        Real      run_time = ParallelDescriptor::second() - strt_time;

#ifdef BL_LAZY
        Lazy::QueueReduction( [=] () mutable {
#endif
        ParallelDescriptor::ReduceRealMax(run_time, IOProc);

        amrex::Print() << "Castro::react_state() time = " << run_time << std::endl << std::endl;
#ifdef BL_LAZY
        });
#endif

    }

    if (burn_success) {
        return true;
    } else {
        return false;
    }

}
#endif


bool
Castro::valid_zones_to_burn(MultiFab& State)
{

    // The default values of the limiters are 0 and 1.e200, respectively.

    Real small = 1.e-10;
    Real large = 1.e199;

    // Check whether we are limiting on either rho or T.

    bool limit_small_rho = react_rho_min >= small;
    bool limit_large_rho = react_rho_max <= large;

    bool limit_rho = limit_small_rho || limit_large_rho;

    bool limit_small_T = react_T_min >= small;
    bool limit_large_T = react_T_max <= large;

    bool limit_T = limit_small_T || limit_large_T;

    bool limit = limit_rho || limit_T;

    if (!limit) {
      return true;
    }

    // Now, if we're limiting on rho, collect the
    // minimum and/or maximum and compare.

    amrex::Vector<Real> small_limiters;
    amrex::Vector<Real> large_limiters;

    bool local = true;

    Real smalldens = small;
    Real largedens = large;

    if (limit_small_rho) {
      smalldens = State.min(URHO, 0, local);
      small_limiters.push_back(smalldens);
    }

    if (limit_large_rho) {
      largedens = State.max(URHO, 0, local);
      large_limiters.push_back(largedens);
    }

    Real small_T = small;
    Real large_T = large;

    if (limit_small_T) {
      small_T = State.min(UTEMP, 0, local);
      small_limiters.push_back(small_T);
    }

    if (limit_large_T) {
      large_T = State.max(UTEMP, 0, local);
      large_limiters.push_back(large_T);
    }

    // Now do the reductions. We're being careful here
    // to limit the amount of work and communication,
    // because regularly doing this check only makes sense
    // if it is negligible compared to the amount of work
    // needed to just do the burn as normal.

    int small_size = small_limiters.size();

    if (small_size > 0) {
        amrex::ParallelDescriptor::ReduceRealMin(small_limiters.dataPtr(), small_size);

        if (limit_small_rho) {
            smalldens = small_limiters[0];
            if (limit_small_T) {
                small_T = small_limiters[1];
            }
        } else {
            small_T = small_limiters[0];
        }
    }

    int large_size = large_limiters.size();

    if (large_size > 0) {
        amrex::ParallelDescriptor::ReduceRealMax(large_limiters.dataPtr(), large_size);

        if (limit_large_rho) {
            largedens = large_limiters[0];
            if (limit_large_T) {
                large_T = large_limiters[1];
            }
        } else {
            large_T = large_limiters[1];
        }
    }

    // Finally check on whether min <= rho <= max
    // and min <= T <= max. The defaults are small
    // and large respectively, so if the limiters
    // are not on, these checks will not be triggered.

    if (largedens >= react_rho_min && smalldens <= react_rho_max &&
        large_T >= react_T_min && small_T <= react_T_max) {
        return true;
    }

    // If we got to this point, we did not survive the limiters,
    // so there are no zones to burn.

    if (verbose > 1) {
        amrex::Print() << "  No valid zones to burn, skipping react_state()." << std::endl;
    }

    return false;

}
