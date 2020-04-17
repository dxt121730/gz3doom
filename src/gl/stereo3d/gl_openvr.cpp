//
//---------------------------------------------------------------------------
//
// Copyright(C) 2016-2017 Christopher Bruns
// All rights reserved.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program.  If not, see http://www.gnu.org/licenses/
//
//--------------------------------------------------------------------------
//
/*
** gl_openvr.cpp
** Stereoscopic virtual reality mode for the HTC Vive headset
**
*/

#ifdef USE_OPENVR


#include <fstream>
#include <sstream>
#include <string>
#include <map>
#include <filesystem>
#include "gl_load/gl_system.h"
#include "doomtype.h" // Printf
#include "d_player.h"
#include "g_game.h" // G_Add...
#include "p_local.h" // P_TryMove
#include "gl/renderer/gl_renderer.h"
#include "gl/renderer/gl_renderbuffers.h"
#include "v_2ddrawer.h" // crosshair
#include "hwrenderer/models/hw_models.h"
#include "g_levellocals.h" // pixelstretch
#include "g_statusbar/sbar.h"
#include <cmath>
#include "c_cvars.h"
#include "cmdlib.h"
#include "LSMatrix.h"
#include "w_wad.h"
#include "m_joy.h"
#include "d_gui.h"
#include "d_event.h"
#include "i_time.h"
#include "hwrenderer/data/flatvertices.h"
#include "hwrenderer/data/hw_viewpointbuffer.h"
#include "info.h"


#include "gl_openvr.h"
#include "openvr_include.h"
using namespace openvr;
namespace fs = std::filesystem;
namespace openvr {
#include "openvr.h"
}



void I_StartupOpenVR();
double P_XYMovement(AActor *mo, DVector2 scroll);
float I_OpenVRGetYaw();

extern class DMenu *CurrentMenu;

#ifdef DYN_OPENVR
// Dynamically load OpenVR

#include "i_module.h"
FModule OpenVRModule{ "OpenVR" };

/** Pointer-to-function type, useful for dynamically getting OpenVR entry points. */
// Derived from global entry at the bottom of openvr_capi.h, plus a few other functions
typedef intptr_t (*LVR_InitInternal)(EVRInitError *peError, EVRApplicationType eType);
typedef void (*LVR_ShutdownInternal)();
typedef bool (*LVR_IsHmdPresent)();
typedef intptr_t (*LVR_GetGenericInterface)(const char *pchInterfaceVersion, EVRInitError *peError);
typedef bool (*LVR_IsRuntimeInstalled)();
typedef const char * (*LVR_GetVRInitErrorAsSymbol)(EVRInitError error);
typedef const char * (*LVR_GetVRInitErrorAsEnglishDescription)(EVRInitError error);
typedef bool (*LVR_IsInterfaceVersionValid)(const char * version);
typedef uint32_t (*LVR_GetInitToken)();

#define DEFINE_ENTRY(name) static TReqProc<OpenVRModule, L##name> name{#name};
DEFINE_ENTRY(VR_InitInternal)
DEFINE_ENTRY(VR_ShutdownInternal)
DEFINE_ENTRY(VR_IsHmdPresent)
DEFINE_ENTRY(VR_GetGenericInterface)
DEFINE_ENTRY(VR_IsRuntimeInstalled)
DEFINE_ENTRY(VR_GetVRInitErrorAsSymbol)
DEFINE_ENTRY(VR_GetVRInitErrorAsEnglishDescription)
DEFINE_ENTRY(VR_IsInterfaceVersionValid)
DEFINE_ENTRY(VR_GetInitToken)

#ifdef _WIN32
#define OPENVRLIB "openvr_api.dll"
#elif defined(__APPLE__)
#define OPENVRLIB "libopenvr_api.dylib"
#else
#define OPENVRLIB "libopenvr_api.so"
#endif

#else
// Non-dynamic loading of OpenVR

// OpenVR Global entry points
S_API intptr_t VR_InitInternal(EVRInitError *peError, EVRApplicationType eType);
S_API void VR_ShutdownInternal();
S_API bool VR_IsHmdPresent();
S_API intptr_t VR_GetGenericInterface(const char *pchInterfaceVersion, EVRInitError *peError);
S_API bool VR_IsRuntimeInstalled();
S_API const char * VR_GetVRInitErrorAsSymbol(EVRInitError error);
S_API const char * VR_GetVRInitErrorAsEnglishDescription(EVRInitError error);
S_API bool VR_IsInterfaceVersionValid(const char * version);
S_API uint32_t VR_GetInitToken();

#endif

EXTERN_CVAR(Int, screenblocks);
EXTERN_CVAR(Float, movebob);
EXTERN_CVAR(Bool, gl_billboard_faces_camera);
EXTERN_CVAR(Int, gl_multisample);
EXTERN_CVAR(Float, vr_vunits_per_meter)
EXTERN_CVAR(Float, vr_floor_offset)

EXTERN_CVAR(Bool, openvr_rightHanded)
EXTERN_CVAR(Bool, openvr_moveFollowsOffHand)
EXTERN_CVAR(Bool, openvr_drawControllers)
EXTERN_CVAR(Float, openvr_weaponRotate);
EXTERN_CVAR(Float, openvr_weaponScale);

CVAR(Float, openvr_kill_momentum, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, openvr_hudDistance, 0.4f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, openvr_hudPitch, -8.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, openvr_fixPitch,false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, openvr_fixRoll, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

const float DEAD_ZONE = 0.25f;


bool IsOpenVRPresent()
{
#ifndef USE_OPENVR
	return false;
#elif !defined DYN_OPENVR
	return true;
#else
	static bool cached_result = false;
	static bool done = false;

	if (!done)
	{
		done = true;
		cached_result = OpenVRModule.Load({ NicePath("$PROGDIR/" OPENVRLIB), OPENVRLIB });
	}
	return cached_result;
#endif
}

void Draw2D(F2DDrawer* drawer, FRenderState& state, bool outside2D);

// feature toggles, for testing and debugging
static const bool doTrackHmdYaw = true;
static const bool doTrackHmdPitch = true;
static const bool doTrackHmdRoll = true;
static const bool doLateScheduledRotationTracking = true;
static const bool doStereoscopicViewpointOffset = true;
static const bool doRenderToDesktop = true; // mirroring to the desktop is very helpful for debugging
static const bool doRenderToHmd = true;
static const bool doTrackHmdVerticalPosition = true;
static const bool doTrackHmdHorizontalPosition = true;
static const bool doTrackVrControllerPosition = false; // todo:

static int axisTrackpad = -1;
static int axisJoystick = -1;
static int axisTrigger = -1;
static bool identifiedAxes = false;

LSVec3 openvr_dpos(0,0,0);
DAngle openvr_to_doom_angle;

namespace s3d
{
	struct vibeFunc {
		int start_time = 0;
		int end_time = 0;
		double coeff = 0;
		int intercept = 0;
	};
	struct vibeDef {
		int duration;
		std::string weaponName;
		bool bPassive = true;
		bool bTicMode = true;
		std::vector<vibeFunc> vibes;
	};
	std::map < std::string,struct vibeDef> vibeDefs;
	std::map < std::string,struct vibeDef> vibePassives;
	
	static void LoadVibeDefs()
	{
		std::string path("./Exports/");
		std::string ext(".vibe");
		std::vector<std::string> vibeFiles;
		for (const auto& entry : fs::directory_iterator(path))
			vibeFiles.push_back(entry.path().string());
		for each (std::string pathStr in vibeFiles)
		{
			std::ifstream inFile;
			if (pathStr.find(ext) == std::string::npos)
				continue;
			inFile.open(pathStr);
			if (!inFile) {
				continue;
			}
			else {
				vibeDef vibe;
				std::string currLine;
				while (std::getline(inFile, currLine))
				{
					std::istringstream lineStream(currLine);
					std::string str;

					std::vector<std::string> strVec;
					//int i = 0;
					while (std::getline(lineStream, str, ';'))
					{
						str.erase(std::remove_if(str.begin(), str.end(), ::isspace), str.end());
						strVec.push_back(str);
					}
					if (strVec[0]._Equal("NAME"))
					{
						if (strVec.size() != 2)
							continue;
						vibe.weaponName = strVec[1];
					}
					else if (strVec[0]._Equal("DURATION")) {
						if (strVec.size() != 2)
							continue;
						vibe.duration = std::stoi(strVec[1]);
					}
					else if (strVec[0]._Equal("PASSIVE")) {
						if (strVec.size() != 2)
							continue;
						vibe.bPassive = strVec[1]._Equal("TRUE");
					}
					else if (strVec[0]._Equal("TICS")) {
						if (strVec.size() != 2)
							continue;
						vibe.bTicMode = strVec[1]._Equal("TRUE");
					}
					else if (strVec[0]._Equal("VIBEDEF")) {
						if (strVec.size() < 5)
							continue;
						vibeFunc func;
						func.start_time = std::stoi(strVec[1]);
						func.end_time = std::stoi(strVec[2]);
						func.coeff = std::stod(strVec[3]);
						func.intercept = std::stoi(strVec[4]);
						vibe.vibes.push_back(func);
					}
				}
				if (!vibe.bPassive)
				{
					vibeDefs[vibe.weaponName] = vibe;

				}

				else
				{
					vibePassives[vibe.weaponName] = vibe;

				}
			}
			inFile.close();
		}

	}
	

	int lastVibe = 0;
	int weaponVibe = -9999;
	int lastRefire = 0;

	static VR_IVRSystem_FnTable* vr_ptr;
	static LSVec3 openvr_origin(0, 0, 0);
	static float deltaYawDegrees;
	
	
	class FControllerTexture : public FTexture
	{
	public:
		FControllerTexture(RenderModel_TextureMap_t* tex) : FTexture()
		{
			m_pTex = tex;
			Width = m_pTex->unWidth;
			Height = m_pTex->unHeight;
		}

		/*const uint8_t *GetColumn(FRenderStyle style, unsigned int column, const Span **spans_out)
		{
			return nullptr;
		}*/
		const uint8_t *GetPixels(FRenderStyle style)
		{
			return m_pTex->rubTextureMapData;
		}

		RenderModel_TextureMap_t* m_pTex;
	};

class VRControllerModel : public FModel
{
public:
	enum LoadState {
		LOADSTATE_INITIAL,
		LOADSTATE_LOADING_VERTICES,
		LOADSTATE_LOADING_TEXTURE,
		LOADSTATE_LOADED,
		LOADSTATE_ERROR
	};

	VRControllerModel(const std::string& model_name, VR_IVRRenderModels_FnTable * vrRenderModels)
		: loadState(LOADSTATE_INITIAL)
		, modelName(model_name)
		, vrRenderModels(vrRenderModels)
	{
		if (!vrRenderModels) {
			loadState = LOADSTATE_ERROR;
			return;
		}
		isLoaded();
	}
	VRControllerModel() {}

	// FModel methods

	virtual bool Load(const char * fn, int lumpnum, const char * buffer, int length) override {
		return false;
	}

	// Controller models don't have frames so always return 0
	virtual int FindFrame(const char * name) override {
		return 0;
	}

	virtual void RenderFrame(FModelRenderer* renderer, FTexture * skin, int frame, int frame2, double inter, int translation = 0)  override
	{
		if (!isLoaded())
			return;
		FMaterial * tex = FMaterial::ValidateTexture(pFTex, false);
		auto vbuf = GetVertexBuffer(renderer);
		vbuf->SetupFrame(renderer, 0, 0, 0);
		renderer->SetMaterial(pFTex, CLAMP_NONE, translation);
		renderer->DrawElements(pModel->unTriangleCount * 3, 0);
	}

	virtual void BuildVertexBuffer(FModelRenderer* renderer) override
	{
		if (loadState != LOADSTATE_LOADED)
			return;

		auto vbuf = GetVertexBuffer(renderer);
		if (vbuf != NULL)
			return;

		vbuf = new FModelVertexBuffer(true, true);
		FModelVertex *vertptr = vbuf->LockVertexBuffer(pModel->unVertexCount);
		unsigned int *indxptr = vbuf->LockIndexBuffer(pModel->unTriangleCount * 3);

		for (int v = 0; v < pModel->unVertexCount; ++v)
		{
			const RenderModel_Vertex_t & vd = pModel->rVertexData[v];
			vertptr[v].x = vd.vPosition.v[0];
			vertptr[v].y = vd.vPosition.v[1];
			vertptr[v].z = vd.vPosition.v[2];
			vertptr[v].u = vd.rfTextureCoord[0];
			vertptr[v].v = vd.rfTextureCoord[1];
			vertptr[v].SetNormal(
				vd.vNormal.v[0],
				vd.vNormal.v[1],
				vd.vNormal.v[2]);
		}
		for (int i = 0; i < pModel->unTriangleCount * 3; ++i)
		{
			indxptr[i] = pModel->rIndexData[i];
		}

		vbuf->UnlockVertexBuffer();
		vbuf->UnlockIndexBuffer();
		SetVertexBuffer(renderer, vbuf);
	}

	virtual void AddSkins(uint8_t *hitlist) override 
	{
	
	}

	bool isLoaded()
	{
		if (loadState == LOADSTATE_ERROR)
			return false;
		if (loadState == LOADSTATE_LOADED)
			return true;
		if ((loadState == LOADSTATE_INITIAL) || (loadState == LOADSTATE_LOADING_VERTICES))
		{
			// Load vertex data first
			EVRRenderModelError eError = vrRenderModels->LoadRenderModel_Async(const_cast<char *>(modelName.c_str()), &pModel);
			if (eError == EVRRenderModelError_VRRenderModelError_Loading) {
				loadState = LOADSTATE_LOADING_VERTICES;
				return false;
			}
			else if (eError == EVRRenderModelError_VRRenderModelError_None) {
				loadState = LOADSTATE_LOADING_TEXTURE;
				vrRenderModels->LoadTexture_Async(pModel->diffuseTextureId, &pTexture);
			}
			else {
				loadState = LOADSTATE_ERROR;
				return false;
			}
		}
		// Load texture data second
		EVRRenderModelError eError = vrRenderModels->LoadTexture_Async(pModel->diffuseTextureId, &pTexture);
		if (eError == EVRRenderModelError_VRRenderModelError_Loading) {
			return false; // No change, and not done, still loading texture
		}
		if (eError == EVRRenderModelError_VRRenderModelError_None) {
			loadState = LOADSTATE_LOADED;

			pFTex = new FControllerTexture(pTexture);

			auto* di = HWDrawInfo::StartDrawInfo(nullptr, r_viewpoint, nullptr);
			FGLModelRenderer renderer(di, gl_RenderState, -1);
			BuildVertexBuffer(&renderer);
			di->EndDrawInfo();
			return true;
		}
		loadState = LOADSTATE_ERROR;
		return false;
	}

private:
	RenderModel_t * pModel;
	RenderModel_TextureMap_t * pTexture;
	FTexture* pFTex;
	LoadState loadState;
	std::string modelName;
	VR_IVRRenderModels_FnTable * vrRenderModels;

};

static std::map<std::string, VRControllerModel> controllerMeshes;
struct Controller
{
	bool active = false;
	TrackedDevicePose_t pose;
	VRControllerState_t lastState;
	VRControllerModel* model = nullptr;
};

enum { MAX_ROLES = 2 };
Controller controllers[MAX_ROLES];


/* static */
//const VRMode& OpenVRMode::getInstance()
//{
//		static OpenVRMode instance;
//		if (!instance.hmdWasFound)
//			return  *instance.GetVRMode(false);
//		return instance;
//}

static HmdVector3d_t eulerAnglesFromQuat(HmdQuaternion_t quat) {
	double q0 = quat.w;
	// permute axes to make "Y" up/yaw
	double q2 = quat.x;
	double q3 = quat.y;
	double q1 = quat.z;

	// http://stackoverflow.com/questions/18433801/converting-a-3x3-matrix-to-euler-tait-bryan-angles-pitch-yaw-roll
	double roll = atan2(2 * (q0*q1 + q2*q3), 1 - 2 * (q1*q1 + q2*q2));
	double pitch = asin(2 * (q0*q2 - q3*q1));
	double yaw = atan2(2 * (q0*q3 + q1*q2), 1 - 2 * (q2*q2 + q3*q3));

	return HmdVector3d_t{ yaw, pitch, roll };
}

static HmdQuaternion_t quatFromMatrix(HmdMatrix34_t matrix) {
	HmdQuaternion_t q;
	typedef float f34[3][4];
	f34& a = matrix.m;
	// http://www.euclideanspace.com/maths/geometry/rotations/conversions/matrixToQuaternion/
	float trace = a[0][0] + a[1][1] + a[2][2]; // I removed + 1.0f; see discussion with Ethan
	if (trace > 0) {// I changed M_EPSILON to 0
		float s = 0.5f / sqrtf(trace + 1.0f);
		q.w = 0.25f / s;
		q.x = (a[2][1] - a[1][2]) * s;
		q.y = (a[0][2] - a[2][0]) * s;
		q.z = (a[1][0] - a[0][1]) * s;
	}
	else {
		if (a[0][0] > a[1][1] && a[0][0] > a[2][2]) {
			float s = 2.0f * sqrtf(1.0f + a[0][0] - a[1][1] - a[2][2]);
			q.w = (a[2][1] - a[1][2]) / s;
			q.x = 0.25f * s;
			q.y = (a[0][1] + a[1][0]) / s;
			q.z = (a[0][2] + a[2][0]) / s;
		}
		else if (a[1][1] > a[2][2]) {
			float s = 2.0f * sqrtf(1.0f + a[1][1] - a[0][0] - a[2][2]);
			q.w = (a[0][2] - a[2][0]) / s;
			q.x = (a[0][1] + a[1][0]) / s;
			q.y = 0.25f * s;
			q.z = (a[1][2] + a[2][1]) / s;
		}
		else {
			float s = 2.0f * sqrtf(1.0f + a[2][2] - a[0][0] - a[1][1]);
			q.w = (a[1][0] - a[0][1]) / s;
			q.x = (a[0][2] + a[2][0]) / s;
			q.y = (a[1][2] + a[2][1]) / s;
			q.z = 0.25f * s;
		}
	}

	return q;
}

static HmdVector3d_t eulerAnglesFromMatrix(HmdMatrix34_t mat) {
	return eulerAnglesFromQuat(quatFromMatrix(mat));
}

OpenVREyePose::OpenVREyePose(int eye, float shiftFactor, float scaleFactor)
	: VREyeInfo(0.0f, 1.f)
	, eye(eye)
	, eyeTexture(nullptr)
	, currentPose(nullptr)
{
}


/* virtual */
OpenVREyePose::~OpenVREyePose() 
{
	dispose();
}

static void vSMatrixFromHmdMatrix34(VSMatrix& m1, const HmdMatrix34_t& m2)
{
	float tmp[16];
	for (int i = 0; i < 3; ++i) {
		for (int j = 0; j < 4; ++j) {
			tmp[4 * i + j] = m2.m[i][j];
		}
	}
	int i = 3;
	for (int j = 0; j < 4; ++j) {
		tmp[4 * i + j] = 0;
	}
	tmp[15] = 1;
	m1.loadMatrix(&tmp[0]);
}


/* virtual */
DVector3 OpenVREyePose::GetViewShift(FLOATTYPE yaw) const
{

	if (currentPose == nullptr)
		return { 0, 0, 0 };
	const TrackedDevicePose_t& hmd = *currentPose;
	if (! hmd.bDeviceIsConnected)
		return { 0, 0, 0 };
	if (! hmd.bPoseIsValid)
		return { 0, 0, 0 };

	if (! doStereoscopicViewpointOffset)
		return { 0, 0, 0 };

	const HmdMatrix34_t& hmdPose = hmd.mDeviceToAbsoluteTracking;

	// Pitch and Roll are identical between OpenVR and Doom worlds.
	// But yaw can differ, depending on starting state, and controller movement.
	float doomYawDegrees = yaw;
	float openVrYawDegrees = RAD2DEG(-eulerAnglesFromMatrix(hmdPose).v[0]);
	deltaYawDegrees = doomYawDegrees - openVrYawDegrees;
	while (deltaYawDegrees > 180)
		deltaYawDegrees -= 360;
	while (deltaYawDegrees < -180)
		deltaYawDegrees += 360;

	openvr_to_doom_angle = DAngle(-deltaYawDegrees);

	// extract rotation component from hmd transform
	LSMatrix44 openvr_X_hmd(hmdPose);
	LSMatrix44 hmdRot = openvr_X_hmd.getWithoutTranslation(); // .transpose();

	/// In these eye methods, just get local inter-eye stereoscopic shift, not full position shift ///

	// compute local eye shift
	LSMatrix44 eyeShift2;
	eyeShift2.loadIdentity();
	eyeShift2 = eyeShift2 * eyeToHeadTransform; // eye to head
	eyeShift2 = eyeShift2 * hmdRot; // head to openvr

	LSVec3 eye_EyePos = LSVec3(0, 0, 0); // eye position in eye frame
	LSVec3 hmd_EyePos = LSMatrix44(eyeToHeadTransform) * eye_EyePos;
	LSVec3 hmd_HmdPos = LSVec3(0, 0, 0); // hmd position in hmd frame
	LSVec3 openvr_EyePos = openvr_X_hmd * hmd_EyePos;
	LSVec3 openvr_HmdPos = openvr_X_hmd * hmd_HmdPos;
	LSVec3 hmd_OtherEyePos = LSMatrix44(otherEyeToHeadTransform) * eye_EyePos;
	LSVec3 openvr_OtherEyePos = openvr_X_hmd * hmd_OtherEyePos;
	LSVec3 openvr_EyeOffset = openvr_EyePos - openvr_HmdPos;

	VSMatrix doomInOpenVR = VSMatrix();
	doomInOpenVR.loadIdentity();
	// permute axes
	float permute[] = { // Convert from OpenVR to Doom axis convention, including mirror inversion
		-1,  0,  0,  0, // X-right in OpenVR -> X-left in Doom
			0,  0,  1,  0, // Z-backward in OpenVR -> Y-backward in Doom
			0,  1,  0,  0, // Y-up in OpenVR -> Z-up in Doom
			0,  0,  0,  1};
	doomInOpenVR.multMatrix(permute);
	doomInOpenVR.scale(vr_vunits_per_meter, vr_vunits_per_meter, vr_vunits_per_meter); // Doom units are not meters
	double pixelstretch = level.info ? level.info->pixelstretch : 1.2;
	doomInOpenVR.scale(pixelstretch, pixelstretch, 1.0); // Doom universe is scaled by 1990s pixel aspect ratio
	doomInOpenVR.rotate(deltaYawDegrees, 0, 0, 1);

	LSVec3 doom_EyeOffset = LSMatrix44(doomInOpenVR) * openvr_EyeOffset;

	if (doTrackHmdVerticalPosition) {
		// In OpenVR, the real world floor level is at y==0
		// In Doom, the virtual player foot level is viewheight below the current viewpoint (on the Z axis)
		// We want to align those two heights here
		const player_t & player = players[consoleplayer];
		double vh = player.viewheight; // Doom thinks this is where you are
		double hh = (openvr_X_hmd[1][3] - vr_floor_offset) * vr_vunits_per_meter; // HMD is actually here
		doom_EyeOffset[2] += hh - vh;
		// TODO: optionally allow player to jump and crouch by actually jumping and crouching
	}

	if (doTrackHmdHorizontalPosition) {
		// shift viewpoint when hmd position shifts
		static bool is_initial_origin_set = false;
		if (! is_initial_origin_set) {
			// initialize origin to first noted HMD location
			// TODO: implement recentering based on a CCMD
			openvr_origin = openvr_HmdPos;
			is_initial_origin_set = true;
		}
		openvr_dpos = openvr_HmdPos - openvr_origin;

		LSVec3 doom_dpos = LSMatrix44(doomInOpenVR) * openvr_dpos;
		doom_EyeOffset[0] += doom_dpos[0];
		doom_EyeOffset[1] += doom_dpos[1];
	}

	return { doom_EyeOffset[0], doom_EyeOffset[1], doom_EyeOffset[2] };
}

/* virtual */
VSMatrix OpenVREyePose::GetProjection(FLOATTYPE fov, FLOATTYPE aspectRatio, FLOATTYPE fovRatio) const
{
	// Ignore those arguments and get the projection from the SDK
	// VSMatrix vs1 = ShiftedEyePose::GetProjection(fov, aspectRatio, fovRatio);
	return projectionMatrix;
}

void OpenVREyePose::initialize(VR_IVRSystem_FnTable * vrsystem)
{
	float zNear = 5.0;
	float zFar = 65536.0;
	HmdMatrix44_t projection = vrsystem->GetProjectionMatrix(
			EVREye(eye), zNear, zFar);
	HmdMatrix44_t proj_transpose;
	for (int i = 0; i < 4; ++i) {
		for (int j = 0; j < 4; ++j) {
			proj_transpose.m[i][j] = projection.m[j][i];
		}
	}
	projectionMatrix.loadIdentity();
	projectionMatrix.multMatrix(&proj_transpose.m[0][0]);

	HmdMatrix34_t eyeToHead = vrsystem->GetEyeToHeadTransform(EVREye(eye));
	vSMatrixFromHmdMatrix34(eyeToHeadTransform, eyeToHead);
	HmdMatrix34_t otherEyeToHead = vrsystem->GetEyeToHeadTransform(eye == EVREye_Eye_Left ? EVREye_Eye_Right : EVREye_Eye_Left);
	vSMatrixFromHmdMatrix34(otherEyeToHeadTransform, otherEyeToHead);

	if (eyeTexture == nullptr)
		eyeTexture = new Texture_t();
	eyeTexture->handle = nullptr; // TODO: populate this at resolve time
	eyeTexture->eType = ETextureType_TextureType_OpenGL;
	eyeTexture->eColorSpace = EColorSpace_ColorSpace_Linear;
}

void OpenVREyePose::dispose()
{
	if (eyeTexture) {
		delete eyeTexture;
		eyeTexture = nullptr;
	}
}

bool OpenVREyePose::submitFrame(VR_IVRCompositor_FnTable * vrCompositor) const
{
	if (eyeTexture == nullptr)
		return false;
	if (vrCompositor == nullptr)
		return false;
 
	// Copy HDR game texture to local vr LDR framebuffer, so gamma correction could work
	if (eyeTexture->handle == nullptr) {
		glGenFramebuffers(1, &framebuffer);
		glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);

		GLuint texture;
		glGenTextures(1, &texture);
		eyeTexture->handle = (void *)(std::ptrdiff_t)texture;
		glBindTexture(GL_TEXTURE_2D, texture);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, screen->mSceneViewport.width,
			screen->mSceneViewport.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
		GLenum drawBuffers[1] = {GL_COLOR_ATTACHMENT0};
		glDrawBuffers(1, drawBuffers);
	}
	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
		return false;
	GLRenderer->mBuffers->BindEyeTexture(eye, 0);
	IntRect box = {0, 0, screen->mSceneViewport.width, screen->mSceneViewport.height};
	GLRenderer->DrawPresentTexture(box, true);

	// Maybe this would help with AMD boards?
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glBindTexture(GL_TEXTURE_2D, 0);

	static VRTextureBounds_t tBounds = {0, 0, 1, 1};
	vrCompositor->Submit(EVREye(eye), eyeTexture, &tBounds, EVRSubmitFlags_Submit_Default);
	return true;
}

VSMatrix OpenVREyePose::getQuadInWorld(
	float distance, // meters
	float width, // meters 
	bool doFixPitch,
	bool doFixRoll,
	float pitchOffset) const 
{
	VSMatrix new_projection;
	new_projection.loadIdentity();

	// doom_units from meters
	new_projection.scale(
		-vr_vunits_per_meter,
		vr_vunits_per_meter,
		-vr_vunits_per_meter);
	double pixelstretch = level.info ? level.info->pixelstretch : 1.2;
	new_projection.scale(pixelstretch, pixelstretch, 1.0); // Doom universe is scaled by 1990s pixel aspect ratio

	const OpenVREyePose * activeEye = this;

	// eye coordinates from hmd coordinates
	LSMatrix44 e2h(activeEye->eyeToHeadTransform);
	new_projection.multMatrix(e2h.transpose());

	// Follow HMD orientation, EXCEPT for roll angle (keep weapon upright)
	if (activeEye->currentPose) {
		if (doFixRoll) {
			float openVrRollDegrees = RAD2DEG(-eulerAnglesFromMatrix(activeEye->currentPose->mDeviceToAbsoluteTracking).v[2]);
			new_projection.rotate(-openVrRollDegrees, 0, 0, 1);
		}

		if (doFixPitch) {
			float openVrPitchDegrees = RAD2DEG(-eulerAnglesFromMatrix(activeEye->currentPose->mDeviceToAbsoluteTracking).v[1]);
			new_projection.rotate(-openVrPitchDegrees, 1, 0, 0);
		}
		if (pitchOffset != 0)
			new_projection.rotate(-pitchOffset, 1, 0, 0);
	}

	// hmd coordinates (meters) from ndc coordinates
	// const float weapon_distance_meters = 0.55f;
	// const float weapon_width_meters = 0.3f;
	const float aspect = SCREENWIDTH / float(SCREENHEIGHT);
	new_projection.translate(0.0, 0.0, distance);
	new_projection.scale(
		-width,
		width / aspect,
		-width);

	// ndc coordinates from pixel coordinates
	new_projection.translate(-1.0, 1.0, 0);
	new_projection.scale(2.0 / SCREENWIDTH, -2.0 / SCREENHEIGHT, -1.0);

	VSMatrix proj(activeEye->projectionMatrix);
	proj.multMatrix(new_projection);
	new_projection = proj;

	return new_projection;
}

void ApplyVPUniforms(HWDrawInfo* di)
{
	di->VPUniforms.CalcDependencies();
	di->vpIndex = screen->mViewpoints->SetViewpoint(gl_RenderState, &di->VPUniforms);
}

void OpenVREyePose::AdjustHud() const
{
	// Draw crosshair on a separate quad, before updating HUD matrix
	const auto vrmode = VRMode::GetVRMode(true);
	if (vrmode->mEyeCount == 1)
	{
		return;
	}
	auto *di = HWDrawInfo::StartDrawInfo(nullptr, r_viewpoint, nullptr);

	di->VPUniforms.mViewMatrix.loadIdentity();
	const OpenVRMode * openVrMode = static_cast<const OpenVRMode *>(vrmode);
	if (openVrMode 
		&& openVrMode->crossHairDrawer
		// Don't draw the crosshair if there is none
		&& CrosshairImage != NULL 
		&& gamestate != GS_TITLELEVEL 
		&& r_viewpoint.camera->health > 0)
	{
		const float crosshair_distance_meters = 10.0f; // meters
		const float crosshair_width_meters = 0.2f * crosshair_distance_meters;
		di->VPUniforms.mProjectionMatrix = getQuadInWorld(
			crosshair_distance_meters,
			crosshair_width_meters,
			openvr_fixPitch,
			openvr_fixRoll,
			0.0);
		ApplyVPUniforms(di);
		::Draw2D(openVrMode->crossHairDrawer, gl_RenderState, true);
	}

	// Update HUD matrix to render on a separate quad
	const float menu_distance_meters = 1.f;
	const float menu_width_meters = openvr_hudDistance * menu_distance_meters;
	const float pitch_offset = openvr_hudPitch;
	di->VPUniforms.mProjectionMatrix = getQuadInWorld(
		menu_distance_meters, 
		menu_width_meters, 
		openvr_fixPitch,
		openvr_fixRoll,
		pitch_offset);
	ApplyVPUniforms(di);
	di->EndDrawInfo();
}

void OpenVREyePose::AdjustBlend(HWDrawInfo *di) const
{
	bool new_di = false;
	if (di == nullptr)
	{
		di = HWDrawInfo::StartDrawInfo(nullptr, r_viewpoint, nullptr);
		new_di = true;
	}

	VSMatrix& proj = di->VPUniforms.mProjectionMatrix;
	proj.loadIdentity();
	proj.translate(-1, 1, 0);
	proj.scale(2.0 / SCREENWIDTH, -2.0 / SCREENHEIGHT, -1.0);
	ApplyVPUniforms(di);

	if (new_di)
	{
		di->EndDrawInfo();
	}		
}

OpenVRMode::OpenVRMode(OpenVREyePose eyes[2])
	: VRMode(2, 1.f, 1.f, 1.f, eyes)
	, vrSystem(nullptr)
	, hmdWasFound(false)
	, sceneWidth(0), sceneHeight(0)
	, vrCompositor(nullptr)
	, vrRenderModels(nullptr)
	, vrToken(0)
	, crossHairDrawer(new F2DDrawer)
{
	//eye_ptrs.Push(&leftEyeView); // initially default behavior to Mono non-stereo rendering
	LoadVibeDefs();
	leftEyeView = &eyes[0];
	rightEyeView = &eyes[1];
	mEyes[0] = &eyes[0];
	mEyes[1] = &eyes[1];

	if ( ! IsOpenVRPresent() ) return; // failed to load openvr API dynamically

	if ( ! VR_IsRuntimeInstalled() ) return; // failed to find OpenVR implementation

	if ( ! VR_IsHmdPresent() ) return; // no VR headset is attached

	EVRInitError eError;
	// Code below recapitulates the effects of C++ call vr::VR_Init()
	VR_InitInternal(&eError, EVRApplicationType_VRApplication_Scene);
	if (eError != EVRInitError_VRInitError_None) {
		std::string errMsg = VR_GetVRInitErrorAsEnglishDescription(eError);
		return;
	}
	if (! VR_IsInterfaceVersionValid(IVRSystem_Version))
	{
		VR_ShutdownInternal();
		return;
	}
	vrToken = VR_GetInitToken();
	const std::string sys_key = std::string("FnTable:") + std::string(IVRSystem_Version);
	vrSystem = (VR_IVRSystem_FnTable*) VR_GetGenericInterface(sys_key.c_str() , &eError);
	if (vrSystem == nullptr)
		return;
	vr_ptr = vrSystem;
	vrSystem->GetRecommendedRenderTargetSize(&sceneWidth, &sceneHeight);

	leftEyeView->initialize(vrSystem);
	rightEyeView->initialize(vrSystem);


	const std::string comp_key = std::string("FnTable:") + std::string(IVRCompositor_Version);
	vrCompositor = (VR_IVRCompositor_FnTable*)VR_GetGenericInterface(comp_key.c_str(), &eError);
	if (vrCompositor == nullptr)
		return;

	const std::string model_key = std::string("FnTable:") + std::string(IVRRenderModels_Version);
	vrRenderModels = (VR_IVRRenderModels_FnTable*)VR_GetGenericInterface(model_key.c_str(), &eError);

	//eye_ptrs.Push(&rightEyeView); // NOW we render to two eyes
	hmdWasFound = true;

	crossHairDrawer->Clear();
}

/* virtual */
// AdjustViewports() is called from within FLGRenderer::SetOutputViewport(...)
void OpenVRMode::AdjustViewport(DFrameBuffer* screen) const
{
	if (screen == nullptr)
		return;
	// Draw the 3D scene into the entire framebuffer
	screen->mSceneViewport.width = sceneWidth;
	screen->mSceneViewport.height = sceneHeight;
	screen->mSceneViewport.left = 0;
	screen->mSceneViewport.top = 0;

	screen->mScreenViewport.width = sceneWidth;
	screen->mScreenViewport.height = sceneHeight;
}

void OpenVRMode::AdjustPlayerSprites(HWDrawInfo* di) const
{

	GetWeaponTransform(&gl_RenderState.mModelMatrix);

	float scale = 0.00125f * openvr_weaponScale;
	gl_RenderState.mModelMatrix.scale(scale, -scale, scale);
	gl_RenderState.mModelMatrix.translate(-viewwidth / 2, -viewheight * 3 / 4, 0.0f);

	gl_RenderState.EnableModelMatrix(true);
}

void OpenVRMode::UnAdjustPlayerSprites() const {

	gl_RenderState.EnableModelMatrix(false);
}

void OpenVRMode::AdjustCrossHair() const
{
	// Remove effect of screenblocks setting on crosshair position
	cachedViewheight = viewheight;
	cachedViewwindowy = viewwindowy;
	viewheight = SCREENHEIGHT;
	viewwindowy = 0;
}

void OpenVRMode::UnAdjustCrossHair() const
{
	viewheight = cachedViewheight;
	viewwindowy = cachedViewwindowy;
}

void OpenVRMode::DrawControllerModels(HWDrawInfo *di, FRenderState& state) const
{
	
	if(!openvr_drawControllers)
		return; 
	FGLModelRenderer renderer(di, state, -1);
	for (int i = 0; i < MAX_ROLES; ++i) 
	{
		if (GetHandTransform(i, &state.mModelMatrix) && controllers[i].model)
		{
			state.EnableModelMatrix(true);

			controllers[i].model->RenderFrame(&renderer, 0, 0, 0, 0);
			state.SetVertexBuffer(screen->mVertexData);

			state.EnableModelMatrix(false);
		}
	}
}


bool OpenVRMode::GetHandTransform(int hand, VSMatrix* mat) const
{
	if (controllers[hand].active)
	{
		mat->loadIdentity();
		if (r_viewpoint.camera->player == nullptr)
		{
			return false;
		}

		APlayerPawn* playermo = r_viewpoint.camera->player->mo;
		DVector3 pos = playermo->InterpolatedPosition(r_viewpoint.TicFrac);

		mat->translate(pos.X, pos.Z, pos.Y);

		mat->scale(vr_vunits_per_meter, vr_vunits_per_meter, -vr_vunits_per_meter);

		mat->rotate(-deltaYawDegrees - 180, 0, 1, 0);

		mat->translate(-openvr_origin.x, -vr_floor_offset, -openvr_origin.z);

		LSMatrix44 handToAbs;
		vSMatrixFromHmdMatrix34(handToAbs, controllers[hand].pose.mDeviceToAbsoluteTracking);

		mat->multMatrix(handToAbs.transpose());

		return true;
	}
	return false;
}

bool OpenVRMode::GetWeaponTransform(VSMatrix* out) const
{
	if (GetHandTransform(openvr_rightHanded ? 1 : 0, out))
	{
		out->rotate(openvr_weaponRotate, 1, 0, 0);
		if (!openvr_rightHanded)
			out->scale(-1.0f, 1.0f, 1.0f);
		return true;
	}
	return false;
}

static DVector3 MapAttackDir(AActor* actor, DAngle yaw, DAngle pitch)
{
	LSMatrix44 mat;
	auto vrmode = VRMode::GetVRMode(true);
	if (!vrmode->GetWeaponTransform(&mat))
	{
		double pc = pitch.Cos();

		DVector3 direction = { pc * yaw.Cos(), pc * yaw.Sin(), -pitch.Sin() };
		return direction;
	}
	double pc = pitch.Cos();

	DVector3 refdirection = { pc * yaw.Cos(), pc * yaw.Sin(), -pitch.Sin() };

	yaw -= actor->Angles.Yaw;
	
	//ignore specified pitch (would need to compensate for auto aim and no (vanilla) Doom weapon varies this)
	//pitch -= actor->Angles.Pitch;
	pitch.Degrees = 0;

	pc = pitch.Cos();

	LSVec3 local = { (float)(pc * yaw.Cos()), (float)(pc * yaw.Sin()), (float)(-pitch.Sin()), 0.0f };
	
	DVector3 dir;
	dir.X = local.x * -mat[2][0] + local.y * -mat[0][0] + local.z * -mat[1][0];
	dir.Y = local.x * -mat[2][2] + local.y * -mat[0][2] + local.z * -mat[1][2];
	dir.Z = local.x * -mat[2][1] + local.y * -mat[0][1] + local.z * -mat[1][1];
	dir.MakeUnit();

	return dir;
}



/* virtual */
void OpenVRMode::Present() const {
	// TODO: For performance, don't render to the desktop screen here
	if (doRenderToDesktop) {
		GLRenderer->mBuffers->BindOutputFB();
		GLRenderer->ClearBorders();

		// Compute screen regions to use for left and right eye views
		int leftWidth = screen->mOutputLetterbox.width / 2;
		int rightWidth = screen->mOutputLetterbox.width - leftWidth;
		IntRect leftHalfScreen = screen->mOutputLetterbox;
		leftHalfScreen.width = leftWidth;
		IntRect rightHalfScreen = screen->mOutputLetterbox;
		rightHalfScreen.width = rightWidth;
		rightHalfScreen.left += leftWidth;

		GLRenderer->mBuffers->BindEyeTexture(0, 0);
		GLRenderer->DrawPresentTexture(leftHalfScreen, true);
		GLRenderer->mBuffers->BindEyeTexture(1, 0);
		GLRenderer->DrawPresentTexture(rightHalfScreen, true);
	}

	if (doRenderToHmd) 
	{
		leftEyeView->submitFrame(vrCompositor);
		rightEyeView->submitFrame(vrCompositor);
	}
}

static int mAngleFromRadians(double radians) 
{
	double m = std::round(65535.0 * radians / (2.0 * M_PI));
	return int(m);
}

void OpenVRMode::updateHmdPose(

	FRenderViewpoint& vp,
	double hmdYawRadians, 
	double hmdPitchRadians, 
	double hmdRollRadians) const 
{
	hmdYaw = hmdYawRadians;
	double hmdpitch = hmdPitchRadians;
	double hmdroll = hmdRollRadians;

	double hmdYawDelta = 0;
	if (doTrackHmdYaw) {
		// Set HMD angle game state parameters for NEXT frame
		static double previousHmdYaw = 0;
		static bool havePreviousYaw = false;
		if (!havePreviousYaw) {
			previousHmdYaw = hmdYaw;
			havePreviousYaw = true;
		}
		hmdYawDelta = hmdYaw - previousHmdYaw;
		G_AddViewAngle(mAngleFromRadians(-hmdYawDelta), true);
		previousHmdYaw = hmdYaw;
	}

	/* */
	// Pitch
	if (doTrackHmdPitch) {
		double pixelstretch = level.info ? level.info->pixelstretch : 1.2;
		double hmdPitchInDoom = -atan(tan(hmdpitch) / pixelstretch);
		double viewPitchInDoom = vp.HWAngles.Pitch.Radians();
		double dPitch = 
			// hmdPitchInDoom
			-hmdpitch
			- viewPitchInDoom;
		G_AddViewPitch(mAngleFromRadians(-dPitch), true);
	}

	// Roll can be local, because it doesn't affect gameplay.
	if (doTrackHmdRoll)
		vp.HWAngles.Roll = RAD2DEG(-hmdroll);

	// Late-schedule update to renderer angles directly, too
	if (doLateScheduledRotationTracking) {
		if (doTrackHmdPitch)
			vp.HWAngles.Pitch = RAD2DEG(-hmdpitch);
		if (doTrackHmdYaw) {
			double viewYaw = r_viewpoint.Angles.Yaw.Degrees + RAD2DEG(hmdYawDelta);
			while (viewYaw <= -180.0) 
				viewYaw += 360.0;
			while (viewYaw > 180.0) 
				viewYaw -= 360.0;
			r_viewpoint.Angles.Yaw = viewYaw;
		}
	}
}

static int GetVRAxisState(VRControllerState_t& state, int vrAxis, int axis)
{
	float pos = axis == 0 ? state.rAxis[vrAxis].x : state.rAxis[vrAxis].y;
	return pos < -DEAD_ZONE ? 1 : pos > DEAD_ZONE ? 2 : 0;
}

void Joy_GenerateUIButtonEvents(int oldbuttons, int newbuttons, int numbuttons, const int *keys)
{
	int changed = oldbuttons ^ newbuttons;
	if (changed != 0)
	{
		event_t ev = { 0, 0, 0, 0, 0, 0, 0 };
		int mask = 1;
		for (int j = 0; j < numbuttons; mask <<= 1, ++j)
		{
			if (changed & mask)
			{
				ev.data1 = keys[j];
				ev.type = EV_GUI_Event;
				ev.subtype = (newbuttons & mask) ? EV_GUI_KeyDown : EV_GUI_KeyUp;
				D_PostEvent(&ev);
			}
		}
	}
}

static void HandleVRAxis(VRControllerState_t& lastState, VRControllerState_t& newState, int vrAxis, int axis, int negativedoomkey, int positivedoomkey, int base)
{
	int keys[] = { negativedoomkey + base, positivedoomkey + base };
	Joy_GenerateButtonEvents(GetVRAxisState(lastState, vrAxis, axis), GetVRAxisState(newState, vrAxis, axis), 2, keys);
}

static void HandleUIVRAxis(VRControllerState_t& lastState, VRControllerState_t& newState, int vrAxis, int axis, ESpecialGUIKeys negativedoomkey, ESpecialGUIKeys positivedoomkey)
{
	int keys[] = { (int)negativedoomkey, (int)positivedoomkey };
	Joy_GenerateUIButtonEvents(GetVRAxisState(lastState, vrAxis, axis), GetVRAxisState(newState, vrAxis, axis), 2, keys);
}

static void HandleUIVRAxes(VRControllerState_t& lastState, VRControllerState_t& newState, int vrAxis, 
	ESpecialGUIKeys xnegativedoomkey, ESpecialGUIKeys xpositivedoomkey, ESpecialGUIKeys ynegativedoomkey, ESpecialGUIKeys ypositivedoomkey)
{
	int oldButtons = abs(lastState.rAxis[vrAxis].x) > abs(lastState.rAxis[vrAxis].y)
		? GetVRAxisState(lastState, vrAxis, 0)
		: GetVRAxisState(lastState, vrAxis, 1) << 2;
	int newButtons = abs(newState.rAxis[vrAxis].x) > abs(newState.rAxis[vrAxis].y)
		? GetVRAxisState(newState, vrAxis, 0)
		: GetVRAxisState(newState, vrAxis, 1) << 2;

	int keys[] = { xnegativedoomkey, xpositivedoomkey, ynegativedoomkey, ypositivedoomkey };

	Joy_GenerateUIButtonEvents(oldButtons, newButtons, 4, keys);
}

static void HandleVRButton(VRControllerState_t& lastState, VRControllerState_t& newState, long long vrindex, int doomkey, int base)
{
	Joy_GenerateButtonEvents((lastState.ulButtonPressed & (1LL << vrindex)) ? 1 : 0, (newState.ulButtonPressed & (1LL << vrindex)) ? 1 : 0, 1, doomkey + base);
}

static void HandleUIVRButton(VRControllerState_t& lastState, VRControllerState_t& newState, long long vrindex, int doomkey)
{
	Joy_GenerateUIButtonEvents((lastState.ulButtonPressed & (1LL << vrindex)) ? 1 : 0, (newState.ulButtonPressed & (1LL << vrindex)) ? 1 : 0, 1, &doomkey);
}



static int GetCurrTime() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

int startFireTic = 0;
int startPassiveTic = 0;

static void ReportDetails(int device, std::string weapName, bool bPassive)
{
	std::map<std::string, vibeDef>::iterator iter = bPassive ? vibePassives.find(weapName) : vibeDefs.find(weapName);
	int currTime = GetCurrTime();
	//std::map<std::string, vibeDef>::iterator vP_iter = vibePassives.find(weapName);
	if ((!bPassive && iter != vibeDefs.end()) || (bPassive && iter != vibePassives.end()))
	{
		Printf(iter->second.weaponName.c_str());
		Printf(iter->second.bPassive ? "\n PASSIVE: TRUE" : " PASSIVE: FALSE");
		Printf(iter->second.bTicMode ? " TICMODE: TRUE" : " TICMODE: FALSE");
		Printf("\n");
		Printf("VIBEDEFS: \n");
		for (int i = 0; i < iter->second.vibes.size(); i++) {
			Printf("VIBEDEF=> START: ");
			Printf(std::to_string(iter->second.vibes[i].start_time).c_str());
			Printf(" END: ");
			Printf(std::to_string(iter->second.vibes[i].end_time).c_str());
			Printf(" COEFF: ");
			Printf(std::to_string(iter->second.vibes[i].coeff).c_str());
			Printf(" INTERCEPT: ");
			Printf(std::to_string(iter->second.vibes[i].intercept).c_str());
			Printf("\n");
		}
		Printf("DONE");
	}
}

static void TriggerWeaponVibe(int device,std::string weapName,bool bPassive)
{
	std::map<std::string, vibeDef>::iterator iter = bPassive? vibePassives.find(weapName) : vibeDefs.find(weapName);
	//int currTime = GetCurrTime();
	
	//std::map<std::string, vibeDef>::iterator vP_iter = vibePassives.find(weapName);
	if ((!bPassive && iter != vibeDefs.end()) || (bPassive && iter != vibePassives.end()))
	{
		int deltaTime;
		if (iter->second.bPassive) {
			deltaTime = double(gametic - startPassiveTic);
			if (!iter->second.bTicMode)
				deltaTime = (deltaTime * 1000) / TICRATE;
			startPassiveTic = deltaTime >= iter->second.duration ? gametic : startPassiveTic;
			//Printf("Passive:%s\n",std::to_string(deltaTime));
		}
		else
		{
			deltaTime = double(gametic - startFireTic);
			if (!iter->second.bTicMode)
				deltaTime = (deltaTime * 1000) / TICRATE;
		}

		double intensity;
		for each (vibeFunc def in iter->second.vibes)
		{
			if (deltaTime <= def.end_time) {
				int deltaTime_relative = deltaTime - def.start_time;
				intensity = def.coeff * deltaTime_relative + def.intercept;
				int int_intensity = int(ceil(intensity));
				if (int_intensity > 3999)
					int_intensity = 3999;
				else if (int_intensity < 0)
					int_intensity = 0;
				if (int_intensity != 0) {
					//Printf("TIC: %s ", std::to_string(gametic).c_str());
					//Printf("MS: %s", std::to_string(deltaTime).c_str());
					//Printf("INT: %s\n", std::to_string(int_intensity).c_str());
				}
					
				vr_ptr->TriggerHapticPulse(device, 0, int_intensity);
				break;
			}
		}
		
	}
}
bool bFiring = false;
bool bhadReloading = false;
std::string lastWeapon;
static void HandleVibrations(int device) 
{

	/*if (players[consoleplayer].damagecount > 0) {
		if (players[consoleplayer].damagecount > 60) {                                                                                                                                        
			vr_ptr->TriggerHapticPulse(device, 0, 3000);
			lastVibe = 3000;
		}
		else if (players[consoleplayer].damagecount > 30) {
			vr_ptr->TriggerHapticPulse(device, 0, 2000);
			lastVibe = 2000;
		}
		else {
			vr_ptr->TriggerHapticPulse(device, 0, 1000);
			lastVibe = 1000;
		}
	}*/

	if ((openvr_rightHanded && device == vr_ptr->GetTrackedDeviceIndexForControllerRole(ETrackedControllerRole::ETrackedControllerRole_TrackedControllerRole_RightHand))
		|| (!openvr_rightHanded && device == vr_ptr->GetTrackedDeviceIndexForControllerRole(ETrackedControllerRole::ETrackedControllerRole_TrackedControllerRole_LeftHand)))

	/*if ((device == vr_ptr->GetTrackedDeviceIndexForControllerRole(ETrackedControllerRole::ETrackedControllerRole_TrackedControllerRole_RightHand))
		|| (device == vr_ptr->GetTrackedDeviceIndexForControllerRole(ETrackedControllerRole::ETrackedControllerRole_TrackedControllerRole_LeftHand)))*/
	{
		

		if (players[consoleplayer].ReadyWeapon != nullptr) 
		{


			/*int i;
			int j;
			Bindings.GetKeysForCommand("reloader", &i, &j);
			if ((players[consoleplayer].oldbuttons & i) == i || (players[consoleplayer].oldbuttons & j) == j)
				Printf("Reloading...");
			else
				Printf(std::to_string(players[consoleplayer].original_oldbuttons).c_str());*/
			//if (players[consoleplayer].mo->Inventory->FindInventory(FName("Reloading")) != NULL)
			/*if(Button_Reload.bDown)
				Printf("Reloading...\n");
			*/
			std::string weaponName = std::string(players[consoleplayer].ReadyWeapon->GetClass()->TypeName.GetChars());
			std::map<std::string, vibeDef>::iterator iter = vibeDefs.find(weaponName);
			if (!weaponName._Equal(lastWeapon)) {
				startFireTic = 0;
				startPassiveTic = 0;
				/*AActor* ammo1 = players[consoleplayer].ReadyWeapon->PointerVar<AActor>(NAME_Ammo1);
				if (ammo1 != nullptr) {
					Printf("TYPE 1: ");
					Printf(ammo1->GetClass()->TypeName);
					int ammo1_amount = ammo1->IntVar(NAME_Amount);
					Printf(" COUNT: ");
					Printf(std::to_string(ammo1_amount).c_str());
					Printf("\n");
				}
				AActor* ammo2 = players[consoleplayer].ReadyWeapon->PointerVar<AActor>(NAME_Ammo2);
				if (ammo2 != nullptr) {
					Printf("TYPE 2: ");
					Printf(ammo2->GetClass()->TypeName);
					int ammo2_amount = ammo2->IntVar(NAME_Amount);
					Printf(" COUNT: ");
					Printf(std::to_string(ammo2_amount).c_str());
					Printf("\n");
				}*/
				//ReportDetails(device, weaponName, false);
				//ReportDetails(device, weaponName, true);
				
			}
				
			lastWeapon = weaponName;
			if ((players[consoleplayer].WeaponState & WF_WEAPONREADY) == WF_WEAPONREADY || players[consoleplayer].refire > lastRefire)
			{
				
				bFiring = false;
				//startTime = GetCurrTime();
				if (players[consoleplayer].refire <= lastRefire) 
				{
					TriggerWeaponVibe(device,weaponName,true);
				}
				//Printf("Ready: Resetting...\n");
			}
			if (players[consoleplayer].attackdown)
			{
				//AActor* ammo2 = players[consoleplayer].ReadyWeapon->PointerVar<AActor>(NAME_Ammo2);
				//if (ammo2 != nullptr) {
				//	/*Printf("TYPE 2: ");
				//	Printf(ammo2->GetClass()->TypeName);*/
				//	int ammo2_amount = ammo2->IntVar(NAME_Amount);
				//	if (ammo2_amount == 1)
				//		Printf("RELOADING...");
				//	else
				//		Printf("AMMO: %s", std::to_string(ammo2_amount).c_str());
				//	/*Printf(" COUNT: ");
				//	Printf(std::to_string(ammo2_amount).c_str());
				//	Printf("\n");*/
				//}

				if ((players[consoleplayer].WeaponState & WF_WEAPONREADY) != WF_WEAPONREADY)// == players[consoleplayer]//->FindStateByString("Fire"))
				{
					if (!bFiring){// && iter != vibeDefs.end()) {
						//reset the vibe
						bFiring = true;
						startFireTic = gametic;

						//===CODE FOR EVENTUAL RELOAD VIBRATION (BD64 STYLE)===
						//AActor* ammo1 = players[consoleplayer].ReadyWeapon->PointerVar<AActor>(NAME_Ammo1);
						//AActor* ammo2 = players[consoleplayer].ReadyWeapon->PointerVar<AActor>(NAME_Ammo2);
						//if (ammo1 != nullptr && ammo2 != nullptr) {
						//	/*Printf("TYPE 2: ");
						//	Printf(ammo2->GetClass()->TypeName);*/
						//	int ammo1_amount = ammo1->IntVar(NAME_Amount);
						//	int ammo2_amount = ammo2->IntVar(NAME_Amount);
						//	if (ammo1_amount > 0 && ammo2_amount == 1)
						//		Printf("RELOADING...");
						//	/*Printf(" COUNT: ");
						//	Printf(std::to_string(ammo2_amount).c_str());
						//	Printf("\n");*/
						//}
						////iter->second.remaining = iter->second.duration;
						
					}
				}
				//Printf("%s\n", players[consoleplayer].ReadyWeapon->GetClass()->TypeName.GetChars());

			}else if (!bhadReloading && players[consoleplayer].mo->FindInventory(FName("Reloading")) != NULL)
			{
				//===CODE FOR EVENTUAL RELOAD VIBRATION (BD64 STYLE)===
				/*bhadReloading = true;
				Printf("RELOADING...");*/
			}
			if (bhadReloading) {
				//===CODE FOR EVENTUAL RELOAD VIBRATION (BD64 STYLE)===
				//bhadReloading = players[consoleplayer].mo->FindInventory(FName("Reloading")) != NULL;
			}
			TriggerWeaponVibe(device, weaponName, false);
			lastRefire = players[consoleplayer].refire;
			//wasAttackDown = players[consoleplayer].attackdown;
			
			
		}
		
	}
	
	
	if (lastVibe > 0) {
		lastVibe -= 100;
		vr_ptr->TriggerHapticPulse(device, 0, lastVibe);
	}
}

static void VibeOut() 
{

}



static void HandleControllerState(int device, int role, VRControllerState_t& newState)
{
	VRControllerState_t& lastState = controllers[role].lastState;

	//trigger (swaps with handedness)
	int controller = openvr_rightHanded ? role : 1 - role;
	
	if (CurrentMenu == nullptr) //the quit menu is cancelled by any normal keypress, so don't generate the fire while in menus 
	{

		HandleVibrations(device);
		HandleVRAxis(lastState, newState, 1, 0, KEY_JOY4, KEY_JOY4, controller * (KEY_PAD_RTRIGGER - KEY_JOY4));
		
	}
	HandleUIVRAxis(lastState, newState, 1, 0, GK_RETURN, GK_RETURN);

	//touchpad
	if (axisTrackpad != -1)
	{
		HandleVRAxis(lastState, newState, axisTrackpad, 0, KEY_PAD_LTHUMB_LEFT, KEY_PAD_LTHUMB_RIGHT, role * (KEY_PAD_RTHUMB_LEFT - KEY_PAD_LTHUMB_LEFT));
		HandleVRAxis(lastState, newState, axisTrackpad, 1, KEY_PAD_LTHUMB_DOWN, KEY_PAD_LTHUMB_UP, role * (KEY_PAD_RTHUMB_DOWN - KEY_PAD_LTHUMB_UP));
		HandleUIVRAxes(lastState, newState, axisTrackpad, GK_LEFT, GK_RIGHT, GK_DOWN, GK_UP);
	}

	//WMR joysticks
	if (axisJoystick != -1)
	{
		HandleVRAxis(lastState, newState, axisJoystick, 0, KEY_JOYAXIS1MINUS, KEY_JOYAXIS1PLUS, role * (KEY_JOYAXIS3PLUS - KEY_JOYAXIS1PLUS));
		HandleVRAxis(lastState, newState, axisJoystick, 1, KEY_JOYAXIS2MINUS, KEY_JOYAXIS2PLUS, role * (KEY_JOYAXIS3PLUS - KEY_JOYAXIS1PLUS));
		HandleUIVRAxes(lastState, newState, axisJoystick, GK_LEFT, GK_RIGHT, GK_DOWN, GK_UP);
	}

	HandleVRButton(lastState, newState, openvr::vr::k_EButton_Grip, KEY_PAD_LSHOULDER, role * (KEY_PAD_RSHOULDER - KEY_PAD_LSHOULDER));
	HandleUIVRButton(lastState, newState, openvr::vr::k_EButton_Grip, GK_BACK);
	HandleVRButton(lastState, newState, openvr::vr::k_EButton_ApplicationMenu, KEY_PAD_START, role * (KEY_PAD_BACK - KEY_PAD_START));
	
	//Extra controls for rift
	HandleVRButton(lastState, newState, openvr::vr::k_EButton_A, KEY_PAD_A, role * (KEY_PAD_B - KEY_PAD_A));
	HandleVRButton(lastState, newState, openvr::vr::k_EButton_SteamVR_Touchpad, KEY_PAD_X, role * (KEY_PAD_Y - KEY_PAD_X));

	lastState = newState;
}

VRControllerState_t& OpenVR_GetState(int hand)
{
	int controller = openvr_rightHanded ? hand : 1 - hand;
	return controllers[controller].lastState;
}


int OpenVR_GetTouchPadAxis()
{
	return axisTrackpad;
}

int OpenVR_GetJoystickAxis()
{
	return axisJoystick;
}

bool OpenVR_OnHandIsRight()
{
	return openvr_rightHanded;
}


static inline int joyint(double val)
{
	if (val >= 0)
	{
		return int(ceil(val));
	}
	else
	{
		return int(floor(val));
	}
}

bool JustStoppedMoving(VRControllerState_t& lastState, VRControllerState_t& newState, int axis)
{
	if (axis != -1)
	{
		bool wasMoving = (abs(lastState.rAxis[axis].x) > DEAD_ZONE || abs(lastState.rAxis[axis].y) > DEAD_ZONE);
		bool isMoving = (abs(newState.rAxis[axis].x) > DEAD_ZONE || abs(newState.rAxis[axis].y) > DEAD_ZONE);
		return !isMoving && wasMoving;
	}
	return false;
}

/* virtual */
void OpenVRMode::SetUp() const
{
	super::SetUp();

	if (vrCompositor == nullptr)
		return;

	// Set VR-appropriate settings
	const bool doAdjustVrSettings = true;
	if (doAdjustVrSettings) {
		movebob = 0;
		gl_billboard_faces_camera = true;
		if (gl_multisample < 2)
			gl_multisample = 4;
	}

	if (gamestate == GS_LEVEL) {
		cachedScreenBlocks = screenblocks;
		screenblocks = 12; // always be full-screen during 3D scene render
	}
	else {
		// TODO: Draw a more interesting background behind the 2D screen
		for (int i = 0; i < 2; ++i) {
			GLRenderer->mBuffers->BindEyeFB(i);
			glClearColor(0.3f, 0.1f, 0.1f, 1.0f); // draw a dark red universe
			glClear(GL_COLOR_BUFFER_BIT);
		}
	}

	static TrackedDevicePose_t poses[k_unMaxTrackedDeviceCount];
	vrCompositor->WaitGetPoses(
		poses, k_unMaxTrackedDeviceCount, // current pose
		nullptr, 0 // future pose?
	);

	TrackedDevicePose_t& hmdPose0 = poses[k_unTrackedDeviceIndex_Hmd];

	if (hmdPose0.bPoseIsValid) {
		const HmdMatrix34_t& hmdPose = hmdPose0.mDeviceToAbsoluteTracking;
		HmdVector3d_t eulerAngles = eulerAnglesFromMatrix(hmdPose);
		updateHmdPose(r_viewpoint, eulerAngles.v[0], eulerAngles.v[1], eulerAngles.v[2]);
		leftEyeView->setCurrentHmdPose(&hmdPose0);
		rightEyeView->setCurrentHmdPose(&hmdPose0);

		player_t* player = r_viewpoint.camera ? r_viewpoint.camera->player : nullptr;

		// Check for existence of VR motion controllers...
		for (int i = 0; i < vr::k_unMaxTrackedDeviceCount; ++i) {
			if (i == k_unTrackedDeviceIndex_Hmd)
				continue; // skip the headset position
			TrackedDevicePose_t & pose = poses[i];
			if (! pose.bDeviceIsConnected)
				continue;
			if (! pose.bPoseIsValid)
				continue;
			ETrackedDeviceClass device_class = vrSystem->GetTrackedDeviceClass(i);
			if (device_class != ETrackedDeviceClass_TrackedDeviceClass_Controller)
				continue; // controllers only, please
			
			int role = vrSystem->GetControllerRoleForTrackedDeviceIndex(i) - ETrackedControllerRole_TrackedControllerRole_LeftHand;
			if (role >= 0 && role < MAX_ROLES)
			{
				char model_chars[101];
				ETrackedPropertyError propertyError;
				vrSystem->GetStringTrackedDeviceProperty(i, ETrackedDeviceProperty_Prop_RenderModelName_String, model_chars, 100, &propertyError);
				if (propertyError != ETrackedPropertyError_TrackedProp_Success)
					continue; // something went wrong...
				std::string model_name(model_chars);
				if (controllerMeshes.count(model_name) == 0) {
					controllerMeshes[model_name] = VRControllerModel(model_name, vrRenderModels);
					assert(controllerMeshes.count(model_name) == 1);
				}
				controllers[role].active = true;
				controllers[role].pose = pose;
				if (controllerMeshes[model_name].isLoaded())
				{
					controllers[role].model = &controllerMeshes[model_name];
				}
				VRControllerState_t newState;
				vrSystem->GetControllerState(i, &newState, sizeof(newState));

				if (!identifiedAxes)
				{
					identifiedAxes = true;
					for (int a = 0; a < k_unControllerStateAxisCount; a++)
					{
						switch (vrSystem->GetInt32TrackedDeviceProperty(i, (ETrackedDeviceProperty)(vr::Prop_Axis0Type_Int32 + a), 0))
						{
						case vr::k_eControllerAxis_TrackPad:
							if (axisTrackpad == -1) axisTrackpad = a;
							break;
						case vr::k_eControllerAxis_Joystick:
							if (axisJoystick == -1) axisJoystick = a;
							break;
						case vr::k_eControllerAxis_Trigger:
							if (axisTrigger == -1) axisTrigger = a;
							break;
						}
					}
				}

				if (player && openvr_kill_momentum)
				{
					if (role == (openvr_rightHanded ? 0 : 1))
					{
						if (JustStoppedMoving(controllers[role].lastState, newState, axisTrackpad) 
							|| JustStoppedMoving(controllers[role].lastState, newState, axisJoystick))
						{
							player->mo->Vel[0] = 0;
							player->mo->Vel[1] = 0;
						}
					}
				}
				
				HandleControllerState(i, role, newState);
			}
		}

		LSMatrix44 mat; 
		if (player)
		{
			if (GetWeaponTransform(&mat))
			{
				player->mo->OverrideAttackPosDir = true;

				player->mo->AttackPos.X = mat[3][0];
				player->mo->AttackPos.Y = mat[3][2];
				player->mo->AttackPos.Z = mat[3][1];

				player->mo->AttackDir = MapAttackDir;
			}
			if (GetHandTransform(openvr_rightHanded ? 0 : 1, &mat) && openvr_moveFollowsOffHand)
			{
				player->mo->ThrustAngleOffset = DAngle(RAD2DEG(atan2f(-mat[2][2], -mat[2][0]))) - player->mo->Angles.Yaw;
			}
			else
			{
				player->mo->ThrustAngleOffset = 0.0f;
			}
			auto vel = player->mo->Vel;
			player->mo->Vel = DVector3((DVector2(-openvr_dpos.x, openvr_dpos.z) * vr_vunits_per_meter).Rotated(openvr_to_doom_angle), 0);
			bool wasOnGround = player->mo->Z() <= player->mo->floorz;
			float oldZ = player->mo->Z();
			P_XYMovement(player->mo, DVector2(0, 0));
			
			//if we were on the ground before offsetting, make sure we still are (this fixes not being able to move on lifts)
			if (player->mo->Z() >= oldZ && wasOnGround)
			{
				player->mo->SetZ(player->mo->floorz);
			}
			else
			{
				player->mo->SetZ(oldZ);
			}
			player->mo->Vel = vel;
			openvr_origin += openvr_dpos;
		}
	}

	I_StartupOpenVR();

	//To feel smooth, yaw changes need to accumulate over the (sub) tic (i.e. render frame, not per tic)
	unsigned int time = I_msTime();
	static unsigned int lastTime = time;

	unsigned int delta = time - lastTime;
	lastTime = time;

	G_AddViewAngle(joyint(-1280 * I_OpenVRGetYaw() * delta * 30 / 1000), true);
}

/* virtual */
void OpenVRMode::TearDown() const
{
	if (gamestate == GS_LEVEL) {
		screenblocks = cachedScreenBlocks;
	}
	super::TearDown();
}

/* virtual */
OpenVRMode::~OpenVRMode() 
{
	if (vrSystem != nullptr) {
		VR_ShutdownInternal();
		vrSystem = nullptr;
		vrCompositor = nullptr;
		vrRenderModels = nullptr;
		leftEyeView->dispose();
		rightEyeView->dispose();
	}
	if (crossHairDrawer != nullptr) {
		delete crossHairDrawer;
		crossHairDrawer = nullptr;
	}
}

} /* namespace s3d */

#endif

