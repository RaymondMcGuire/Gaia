#pragma once

#include "VBD_BaseMeshGPU.h"

namespace GAIA {
	struct VBDTetMeshNeoHookeanGPU : VBDBaseTetMeshGPU {
		FloatingTypeGPU lmbd;
		FloatingTypeGPU miu;

		FloatingTypeGPU dampingHydrostatic;
		FloatingTypeGPU dampingDeviatoric;
	};



}