/* ----------------------------------------------------------------------------

 * GTSAM Copyright 2010, Georgia Tech Research Corporation,
 * Atlanta, Georgia 30332-0415
 * All Rights Reserved
 * Authors: Frank Dellaert, et al. (see THANKS for the full author list)

 * See LICENSE for the license information

 * -------------------------------------------------------------------------- */

/**
 * @file NonlinearOptimization.h
 * @date Oct 14, 2010
 * @author Kai Ni
 * @brief Easy interfaces for NonlinearOptimizer
 */

#pragma once

#include <gtsam/nonlinear/Values.h>
#include <gtsam/nonlinear/NonlinearOptimizationParameters.h>

namespace gtsam {

	/**
	 * all the nonlinear optimization methods
	 */
	typedef enum {
		LM,            // Levenberg Marquardt
		GN             // Gaussian-Newton
	} NonlinearOptimizationMethod;

	/**
	 * all the linear solver types
	 */
	typedef enum {
		SEQUENTIAL,      // Sequential elimination
		MULTIFRONTAL,      // Multi-frontal elimination
#if ENABLE_SPCG
		SPCG,			  // Subgraph Preconditioned Conjugate Gradient
#endif
	} LinearSolver;

	/**
	 * optimization that returns the values
	 */
	template<class G>
	Values optimize(const G& graph, const Values& initialEstimate,
			const NonlinearOptimizationParameters& parameters = NonlinearOptimizationParameters(),
			const LinearSolver& solver = MULTIFRONTAL,
			const NonlinearOptimizationMethod& nonlinear_method = LM);

}

#include <gtsam/nonlinear/NonlinearOptimization-inl.h>
