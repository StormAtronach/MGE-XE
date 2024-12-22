#include "ipc/dlshare.h"
#include "ipc/server.h"
#include "mge/distantshader.h"
#include "support/log.h"

#include <cassert>

#include <d3dx9.h>

template<class T> static inline void CleanupIfc(T*& resource) {
	if (resource != nullptr) {
		resource->Release();
		resource = nullptr;
	}
}

namespace IPC {
	Server::Server(HANDLE sharedMem, HANDLE clientProcess, HANDLE rpcStartEvent, HANDLE rpcCompleteEvent) :
		m_sharedMem(sharedMem),
		m_clientProcess(clientProcess),
		m_rpcStartEvent(rpcStartEvent),
		m_rpcCompleteEvent(rpcCompleteEvent),
		m_ipcParameters(nullptr),
		m_freeVecs(),
		m_d3d(nullptr),
		m_device(nullptr),
		m_effect(nullptr),
		m_technique(NULL),
		m_worldParam(NULL),
		m_viewParam(NULL),
		m_projParam(NULL),
		m_eyePosParam(NULL),
		m_occlusionQuery(nullptr),
		m_occlusionSurface(nullptr),
		m_occlusionIndexes(nullptr),
		m_hasOcclusion(false)
	{ }

	Server::~Server() {
		if (m_ipcParameters != nullptr) {
			UnmapViewOfFile(m_ipcParameters);
			m_ipcParameters = nullptr;
		}

		CleanupIfc(m_occlusionIndexes);
		CleanupIfc(m_occlusionSurface);
		CleanupIfc(m_occlusionQuery);
		CleanupIfc(m_effect);
		CleanupIfc(m_device);
		CleanupIfc(m_d3d);

		CleanupHandle(m_sharedMem);
		CleanupHandle(m_clientProcess);
		CleanupHandle(m_rpcStartEvent);
		CleanupHandle(m_rpcCompleteEvent);
	}

	bool Server::complete() {
		if (!SetEvent(m_rpcCompleteEvent)) {
			LOG::winerror("Failed to signal RPC completion");
			return false;
		}

		return true;
	}

	bool Server::init() {
		if (m_ipcParameters != nullptr) {
			UnmapViewOfFile(m_ipcParameters);
			m_ipcParameters = nullptr;
		}

		m_ipcParameters = static_cast<Parameters*>(MapViewOfFile(m_sharedMem, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Parameters)));
		if (m_ipcParameters == nullptr) {
			LOG::winerror("Failed to map IPC parameters shared memory");
			return false;
		}

		if (!initD3D()) {
			UnmapViewOfFile(m_ipcParameters);
			m_ipcParameters = nullptr;

			return false;
		}

		return true;
	}

	bool Server::initD3D() {
		// all occlusion bounding boxes use the same indexes
		constexpr UINT indexBufferSize = 6 * 6 * 2; // 6 faces with 6 indexes each taking 2 bytes
		constexpr WORD indexes[] = {
			// Front face
			4, 5, 6,  5, 7, 6,
			// Back face
			1, 0, 2,  1, 2, 3,
			// Left face
			0, 4, 2,  4, 6, 2,
			// Right face
			5, 1, 7,  1, 3, 7,
			// Top face
			2, 6, 3,  6, 7, 3,
			// Bottom face
			0, 1, 4,  1, 5, 4
		};

		WNDCLASSA wndClass = {
			CS_CLASSDC | CS_HREDRAW | CS_VREDRAW,
			DefWindowProcA,
			0,
			0,
			NULL,
			NULL,
			NULL,
			NULL,
			NULL,
			"mgeHost64",
		};
		D3DPRESENT_PARAMETERS presentParams = { };
		HWND hWnd = NULL;
		HRESULT hr = S_OK;
		WORD* indexBuffer = nullptr;

		// initialize D3D for occlusion testing
		hr = Direct3DCreate9Ex(D3D_SDK_VERSION, &m_d3d);
		if (FAILED(hr)) {
			LOG::logline("Failed to create Direct3D object: %08X", hr);
			return false;
		}

		// create dummy window
		if (!RegisterClassA(&wndClass)) {
			LOG::winerror("Failed to register window class");
			goto registerClassFailed;
		}

		hWnd = CreateWindowA("mgeHost64", "mgeHost64", WS_OVERLAPPEDWINDOW, 0, 0, 0, 0, NULL, NULL, NULL, NULL);
		if (hWnd == NULL) {
			LOG::winerror("Failed to create dummy window");
			goto createWindowFailed;
		}

		presentParams.Windowed = TRUE;
		presentParams.BackBufferFormat = D3DFMT_X8R8G8B8;
		presentParams.BackBufferWidth = 1;
		presentParams.BackBufferHeight = 1;
		presentParams.SwapEffect = D3DSWAPEFFECT_DISCARD;
		presentParams.EnableAutoDepthStencil = TRUE;
		presentParams.AutoDepthStencilFormat = D3DFMT_D24S8;
		hr = m_d3d->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hWnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &presentParams, NULL, &m_device);
		if (FAILED(hr)) {
			LOG::logline("Failed to create D3D device: %08X", hr);
			goto deviceCreateFailed;
		}

		// create any occlusion stuff that we don't need other information for
		hr = m_device->CreateIndexBuffer(indexBufferSize, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_DEFAULT, &m_occlusionIndexes, NULL);
		if (FAILED(hr)) {
			LOG::logline("Failed to create index buffer: %08X", hr);
			goto indexBufferCreateFailed;
		}

		hr = m_occlusionIndexes->Lock(0, indexBufferSize, reinterpret_cast<void**>(&indexBuffer), 0);
		if (FAILED(hr)) {
			LOG::logline("Failed to lock index buffer: %08X", hr);
			goto indexBufferWriteFailed;
		}

		std::memcpy(indexBuffer, indexes, indexBufferSize);
		m_occlusionIndexes->Unlock();

		hr = m_device->CreateQuery(D3DQUERYTYPE_OCCLUSION, &m_occlusionQuery);
		if (FAILED(hr)) {
			LOG::logline("Failed to create occlusion query: %08X", hr);
			goto createQueryFailed;
		}

		m_device->SetRenderState(D3DRS_LIGHTING, FALSE);

		if (initShaders()) {
			return true;
		}

	createQueryFailed:
	indexBufferWriteFailed:
		CleanupIfc(m_occlusionIndexes);
	indexBufferCreateFailed:
		CleanupIfc(m_device);
	deviceCreateFailed:
		DestroyWindow(hWnd);
		hWnd = NULL;
	createWindowFailed:
		UnregisterClassA("mgeHost64", GetModuleHandleA(NULL));
	registerClassFailed:
		CleanupIfc(m_d3d);

		return false;
	}

	bool Server::initShaders() {
		ID3DXBuffer* errors = nullptr;
		CoreModInclude includer;

		// LAA probably not necessary for x64? but we'll set it anyway for consistency
		auto hr = D3DXCreateEffectFromFileA(m_device, "Data Files\\shaders\\core\\XE Occlusion Main.fx", NULL, &includer, D3DXSHADER_OPTIMIZATION_LEVEL3 | D3DXFX_LARGEADDRESSAWARE | D3DXSHADER_DEBUG, NULL, &m_effect, &errors);
		if (FAILED(hr)) {
			LOG::logline("Shader compile failed: %08X", hr);
			if (errors) {
				LOG::logline("%s", errors->GetBufferPointer());
				errors->Release();
			}

			return false;
		}

		m_technique = m_effect->GetTechniqueByName("Occlusion");
		if (m_technique == NULL) {
			LOG::logline("Failed to get shader technique");
			CleanupIfc(m_effect);
			return false;
		}

		hr = m_effect->SetTechnique(m_technique);
		if (FAILED(hr)) {
			LOG::logline("Failed to set shader technique: %08X", hr);
			CleanupIfc(m_effect);
			return false;
		}

		m_worldParam = m_effect->GetParameterByName(NULL, "world");
		m_viewParam = m_effect->GetParameterByName(NULL, "view");
		m_projParam = m_effect->GetParameterByName(NULL, "proj");
		m_eyePosParam = m_effect->GetParameterByName(NULL, "eyePos");

		if (m_worldParam == NULL || m_viewParam == NULL || m_projParam == NULL || m_eyePosParam == NULL) {
			LOG::logline("Failed to get shader parameters");
			CleanupIfc(m_effect);
			return false;
		}

		return true;
	}

	bool Server::listen() {
		while (true) {
			// signal the completion of whatever we were doing before (also signals that we've finished initializing on the first iteration)
			SetEvent(m_rpcCompleteEvent);

			// 0 = client process, 1 = RPC start event
			auto waitResult = WaitForMultipleObjects(2, m_waitHandles, FALSE, INFINITE);
			if (waitResult == WAIT_FAILED) {
				LOG::winerror("Failed to wait for RPC event");
				return false;
			}

			if (waitResult == WAIT_OBJECT_0) {
				LOG::logline("Morrowind process exited; exiting 64-bit host");
				return true;
			}

			switch (m_ipcParameters->command) {
			case Command::None:
				break;
			case Command::AllocVec:
				allocVec();
				break;
			case Command::FreeVec:
				freeVec();
				break;
			case Command::Exit:
				LOG::logline("Host process received exit command");
				return true;
			case Command::UpdateDynVis:
				updateDynVis();
				break;
			case Command::InitDistantStatics:
				initDistantStatics();
				break;
			case Command::InitLandscape:
				initLandscape();
				break;
			case Command::SetWorldSpace:
				setWorldSpace();
				break;
			case Command::GetVisibleMeshesCoarse:
				getVisibleMeshesCoarse();
				break;
			case Command::GetVisibleMeshes:
				getVisibleMeshes();
				break;
			case Command::SortVisibleSet:
				sortVisibleSet();
				break;
			case Command::InitOcclusion:
				initOcclusion();
				break;
			case Command::GenerateOcclusionMask:
				generateOcclusionMask();
				break;
			default:
				LOG::logline("Received unknown command value %u", m_ipcParameters->command);
				break;
			}
		}
	}

	template<typename T>
	Vec<T>& Server::getVec(VecId id) {
		auto pVec = m_vecs[id];
		// when the client requests to allocate a shared vector, there's no way we can communicate a template
		// argument to the server, so all vectors are stored with a dummy type of char. the actual contained
		// type doesn't affect the layout of the vector (as all it holds is a pointer to the elements), so
		// we can freely cast between types without breaking the class itself. we will do an assert to make
		// sure the size of the type we're being told the vector contains matches the size of the type it
		// was told it contained when it was created.
		assert(sizeof(T) == pVec->m_elementBytes);
		return *reinterpret_cast<Vec<T>*>(pVec);
	}

	bool Server::allocVec() {
		auto& params = m_ipcParameters->params.allocVecParams;

		Vec<char>* vec = nullptr;
		VecId id = InvalidVector;
		if (!m_freeVecs.empty()) {
			id = m_freeVecs.front();
			m_freeVecs.pop();
			m_vecs[id] = vec = new Vec<char>(id, nullptr, params.maxCapacityInElements, params.windowSizeInElements, params.elementSize);
		} else {
			id = static_cast<VecId>(m_vecs.size());
			vec = new Vec<char>(id, nullptr, params.maxCapacityInElements, params.windowSizeInElements, params.elementSize);
			m_vecs.push_back(vec);
		}

		m_ipcParameters->params.allocVecParams.id = id;

		if (!(vec->init(m_clientProcess, params) && vec->reserve(params.initialCapacity))) {
			delete vec;
			m_vecs[id] = nullptr;
			// mark this slot free again
			m_freeVecs.push(id);
			return false;
		}

		return true;
	}

	bool Server::freeVec() {
		auto& params = m_ipcParameters->params.freeVecParams;
		params.wasFreed = false;

		auto& vec = m_vecs[params.id];
		if (vec != nullptr) {
			if (!vec->can_free())
				return false;

			delete vec;
			vec = nullptr;
			m_freeVecs.push(params.id);
			params.wasFreed = true;
		}

		return true;
	}

	void Server::updateDynVis() {
		auto& params = m_ipcParameters->params.dynVisParams;
		auto& vec = getVec<DynVisFlag>(params.id);
		for (auto& update : vec) {
			for (auto mesh : DistantLandShare::dynamicVisGroupsServer[update.groupIndex]) {
				mesh->enabled = update.enable;
			}
		}
	}

	bool Server::initOcclusion() {
		constexpr UINT resolutionScale = 4; // quarter-res occlusion

		HRESULT hr;

		auto& params = m_ipcParameters->params.initOcclusionParams;
		params.success = false;

		auto& displayMode = params.displayMode;
		auto width = displayMode.Width / resolutionScale;
		auto height = displayMode.Height / resolutionScale;

		// resize back buffer
		D3DPRESENT_PARAMETERS presentParams = { };
		presentParams.Windowed = TRUE;
		presentParams.BackBufferFormat = D3DFMT_X8R8G8B8;
		presentParams.BackBufferWidth = width;
		presentParams.BackBufferHeight = height;
		presentParams.SwapEffect = D3DSWAPEFFECT_DISCARD;
		presentParams.EnableAutoDepthStencil = TRUE;
		presentParams.AutoDepthStencilFormat = D3DFMT_D24S8;

		hr = m_device->Reset(&presentParams);
		if (FAILED(hr)) {
			LOG::logline("Failed to reset device: %08X", hr);
			return false;
		}

		hr = m_device->CreateRenderTarget(width, height, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &m_occlusionSurface, NULL);
		if (FAILED(hr)) {
			LOG::logline("Failed to create occlusion texture: %08X", hr);
			return false;
		}

		hr = m_device->SetRenderTarget(0, m_occlusionSurface);
		if (FAILED(hr)) {
			LOG::logline("Failed to set render target: %08X", hr);
			return false;
		}

		params.success = true;
		return true;
	}

	bool Server::initDistantStatics() {
		auto& params = m_ipcParameters->params.distantStaticParams;
		auto& distantStatics = getVec<DistantStatic>(params.distantStatics);
		auto& distantSubsets = getVec<DistantSubset>(params.distantSubsets);
		return DistantLandShare::initDistantStaticsServer(distantStatics, distantSubsets, m_device);
	}

	bool Server::initLandscape() {
		auto& params = m_ipcParameters->params.initLandscapeParams;
		return DistantLandShare::initLandscapeServer(getVec<LandscapeBuffers>(params.buffers), params.texWorldColour, m_device);
	}

	void Server::setWorldSpace() {
		auto& params = m_ipcParameters->params.worldSpaceParams;
		params.cellFound = DistantLandShare::setCurrentWorldSpace(params.cellname);
	}

	void Server::getVisibleMeshesCoarse() {
		auto& params = m_ipcParameters->params.meshParams;
		auto& vec = getVec<RenderMesh>(params.visibleSet);
		DistantLandShare::getVisibleMeshesCoarse(vec, params.viewFrustum, params.sort, params.setFlags);
	}

	void Server::getVisibleMeshes() {
		auto& params = m_ipcParameters->params.meshParams;
		auto& vec = getVec<RenderMesh>(params.visibleSet);
		DistantLandShare::getVisibleMeshes(vec, params.viewFrustum, params.viewSphere, params.sort, params.setFlags);
	}

	void Server::sortVisibleSet() {
		auto& params = m_ipcParameters->params.meshParams;
		auto& vec = getVec<RenderMesh>(params.visibleSet);
		DistantLandShare::sortVisibleSet(vec, params.sort);
	}

#define FAIL_RETURN(r, m) if (FAILED(r)) {\
	LOG::logline(m ": %08X", (r));\
	return false;\
}

#define FAIL_DEVEND(r, m) if (FAILED(r)) {\
	LOG::logline(m ": %08X", (r));\
	m_device->EndScene();\
	return false;\
}

#define FAIL_EFFEND(r, m) if (FAILED(r)) {\
	LOG::logline(m ": %08X", (r));\
	m_effect->End();\
	m_device->EndScene();\
	return false;\
}

#define FAIL_PASSEND(r, m) if (FAILED(r)) {\
	LOG::logline(m ": %08X", (r));\
	m_effect->EndPass();\
	m_effect->End();\
	m_device->EndScene();\
	return false;\
}

	bool Server::generateOcclusionMask() {
		static int counter = 0;

		struct Vertex {
			float x, y, z, w; // Position
		};

		HRESULT hr;

		if (m_hasOcclusion) {
			m_device->EndScene();
			m_hasOcclusion = false;
		}

		auto& params = m_ipcParameters->params.occlusionMaskParams;
		auto& vec = getVec<RenderMesh>(params.visibleSet);

		hr = m_device->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_XRGB(0, 255, 0), 1.0f, 0);
		FAIL_RETURN(hr, "Failed to clear occlusion surface");

		hr = m_device->BeginScene();
		FAIL_RETURN(hr, "Failed to begin scene");

		hr = m_effect->SetMatrix(m_worldParam, &params.world);
		FAIL_DEVEND(hr, "Failed to set world matrix");

		hr = m_effect->SetMatrix(m_viewParam, &params.view);
		FAIL_DEVEND(hr, "Failed to set view matrix");

		hr = m_effect->SetMatrix(m_projParam, &params.proj);
		FAIL_DEVEND(hr, "Failed to set projection matrix");

		hr = m_effect->SetFloatArray(m_eyePosParam, params.eyePos, 3);
		FAIL_DEVEND(hr, "Failed to set eye position");

		UINT numPasses;
		hr = m_effect->Begin(&numPasses, 0);
		FAIL_DEVEND(hr, "Failed to begin effect");

		hr = m_effect->BeginPass(0);
		FAIL_EFFEND(hr, "Failed to begin pass");

		hr = m_device->SetVertexDeclaration(DistantLandShare::LandDecl);
		FAIL_PASSEND(hr, "Failed to set vertex declaration");

		VisibleSet occluders((IpcServerVector(vec)));
		occluders.RenderServer(m_device, SIZEOFLANDVERT, DistantLandShare::landscapeBufferMap);

		m_effect->EndPass();
		m_effect->End();

		if (++counter % 1000 == 0) {
			// save a sample
			m_device->EndScene();
			D3DXSaveSurfaceToFileA("occlusion.bmp", D3DXIFF_BMP, m_occlusionSurface, NULL, NULL);

			return true;
		}

		m_hasOcclusion = true;
		return true;
	}
}