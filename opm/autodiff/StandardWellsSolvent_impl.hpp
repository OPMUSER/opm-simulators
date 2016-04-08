/*
  Copyright 2016 SINTEF ICT, Applied Mathematics.
  Copyright 2016 Statoil ASA.

  This file is part of the Open Porous Media project (OPM).

  OPM is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  OPM is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with OPM.  If not, see <http://www.gnu.org/licenses/>.
*/


#include <opm/autodiff/StandardWellsSolvent.hpp>




namespace Opm
{






    StandardWellsSolvent::StandardWellsSolvent(const Wells* wells_arg,
                                               const SolventPropsAdFromDeck& solvent_props)
        : Base(wells_arg)
        , solvent_props_(solvent_props)
    {
    }





    template<class SolutionState, class WellState>
    void
    StandardWellsSolvent::
    computePropertiesForWellConnectionPressures(const SolutionState& state,
                                                const WellState& xw,
                                                const BlackoilPropsAdInterface& fluid,
                                                const std::vector<bool>& active,
                                                const std::vector<PhasePresence>& pc,
                                                std::vector<double>& b_perf,
                                                std::vector<double>& rsmax_perf,
                                                std::vector<double>& rvmax_perf,
                                                std::vector<double>& surf_dens_perf)
    {
        // 1. Compute properties required by computeConnectionPressureDelta().
        //    Note that some of the complexity of this part is due to the function
        //    taking std::vector<double> arguments, and not Eigen objects.
        const int nperf = wells().well_connpos[wells().number_of_wells];
        const int nw = wells().number_of_wells;

        // Compute the average pressure in each well block
        const Vector perf_press = Eigen::Map<const V>(xw.perfPress().data(), nperf);
        Vector avg_press = perf_press*0;
        for (int w = 0; w < nw; ++w) {
            for (int perf = wells().well_connpos[w]; perf < wells().well_connpos[w+1]; ++perf) {
                const double p_above = perf == wells().well_connpos[w] ? state.bhp.value()[w] : perf_press[perf - 1];
                const double p_avg = (perf_press[perf] + p_above)/2;
                avg_press[perf] = p_avg;
            }
        }

        const std::vector<int>& well_cells = wellOps().well_cells;

        // Use cell values for the temperature as the wells don't knows its temperature yet.
        const ADB perf_temp = subset(state.temperature, well_cells);

        // Compute b, rsmax, rvmax values for perforations.
        // Evaluate the properties using average well block pressures
        // and cell values for rs, rv, phase condition and temperature.
        const ADB avg_press_ad = ADB::constant(avg_press);
        std::vector<PhasePresence> perf_cond(nperf);
        for (int perf = 0; perf < nperf; ++perf) {
            perf_cond[perf] = pc[well_cells[perf]];
        }

        const PhaseUsage& pu = fluid.phaseUsage();
        DataBlock b(nperf, pu.num_phases);

        const Vector bw = fluid.bWat(avg_press_ad, perf_temp, well_cells).value();
        if (pu.phase_used[BlackoilPhases::Aqua]) {
            b.col(pu.phase_pos[BlackoilPhases::Aqua]) = bw;
        }

        assert(active[Oil]);
        assert(active[Gas]);
        const ADB perf_rv = subset(state.rv, well_cells);
        const ADB perf_rs = subset(state.rs, well_cells);
        const Vector perf_so =  subset(state.saturation[pu.phase_pos[Oil]].value(), well_cells);
        if (pu.phase_used[BlackoilPhases::Liquid]) {
            const Vector bo = fluid.bOil(avg_press_ad, perf_temp, perf_rs, perf_cond, well_cells).value();
            //const V bo_eff = subset(rq_[pu.phase_pos[Oil] ].b , well_cells).value();
            b.col(pu.phase_pos[BlackoilPhases::Liquid]) = bo;
            // const Vector rssat = fluidRsSat(avg_press, perf_so, well_cells);
            const Vector rssat = fluid.rsSat(ADB::constant(avg_press), ADB::constant(perf_so), well_cells).value();
            rsmax_perf.assign(rssat.data(), rssat.data() + nperf);
        } else {
            rsmax_perf.assign(0.0, nperf);
        }
        V surf_dens_copy = superset(fluid.surfaceDensity(0, well_cells), Span(nperf, pu.num_phases, 0), nperf*pu.num_phases);
        for (int phase = 1; phase < pu.num_phases; ++phase) {
            if ( phase == pu.phase_pos[BlackoilPhases::Vapour]) {
                continue; // the gas surface density is added after the solvent is accounted for.
            }
            surf_dens_copy += superset(fluid.surfaceDensity(phase, well_cells), Span(nperf, pu.num_phases, phase), nperf*pu.num_phases);
        }

        if (pu.phase_used[BlackoilPhases::Vapour]) {
            // Unclear wether the effective or the pure values should be used for the wells
            // the current usage of unmodified properties values gives best match.
            //V bg_eff = subset(rq_[pu.phase_pos[Gas]].b,well_cells).value();
            Vector bg = fluid.bGas(avg_press_ad, perf_temp, perf_rv, perf_cond, well_cells).value();
            Vector rhog = fluid.surfaceDensity(pu.phase_pos[BlackoilPhases::Vapour], well_cells);
            // to handle solvent related
            {

                const Vector bs = solvent_props_.bSolvent(avg_press_ad,well_cells).value();
                //const V bs_eff = subset(rq_[solvent_pos_].b,well_cells).value();

                // number of cells
                const int nc = state.pressure.size();

                const ADB zero = ADB::constant(Vector::Zero(nc));
                const ADB& ss = state.solvent_saturation;
                const ADB& sg = (active[ Gas ]
                                 ? state.saturation[ pu.phase_pos[ Gas ] ]
                                 : zero);

                Selector<double> zero_selector(ss.value() + sg.value(), Selector<double>::Zero);
                Vector F_solvent = subset(zero_selector.select(ss, ss / (ss + sg)),well_cells).value();

                Vector injectedSolventFraction = Eigen::Map<const Vector>(&xw.solventFraction()[0], nperf);

                Vector isProducer = Vector::Zero(nperf);
                Vector ones = Vector::Constant(nperf,1.0);
                for (int w = 0; w < nw; ++w) {
                    if(wells().type[w] == PRODUCER) {
                        for (int perf = wells().well_connpos[w]; perf < wells().well_connpos[w+1]; ++perf) {
                            isProducer[perf] = 1;
                        }
                    }
                }

                F_solvent = isProducer * F_solvent + (ones - isProducer) * injectedSolventFraction;

                bg = bg * (ones - F_solvent);
                bg = bg + F_solvent * bs;

                const Vector& rhos = solvent_props_.solventSurfaceDensity(well_cells);
                rhog = ( (ones - F_solvent) * rhog ) + (F_solvent * rhos);
            }
            b.col(pu.phase_pos[BlackoilPhases::Vapour]) = bg;
            surf_dens_copy += superset(rhog, Span(nperf, pu.num_phases, pu.phase_pos[BlackoilPhases::Vapour]), nperf*pu.num_phases);

            // const Vector rvsat = fluidRvSat(avg_press, perf_so, well_cells);
            const Vector rvsat = fluid.rvSat(ADB::constant(avg_press), ADB::constant(perf_so), well_cells).value();
            rvmax_perf.assign(rvsat.data(), rvsat.data() + nperf);
        } else {
            rvmax_perf.assign(0.0, nperf);
        }

        // b and surf_dens_perf is row major, so can just copy data.
        b_perf.assign(b.data(), b.data() + nperf * pu.num_phases);
        surf_dens_perf.assign(surf_dens_copy.data(), surf_dens_copy.data() + nperf * pu.num_phases);
    }


}
