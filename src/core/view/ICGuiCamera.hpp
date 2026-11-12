#pragma once

#include <string>

#include "Config.hpp"
#include "core/assets/InputCamera.hpp"
#include "ICameraHandler.hpp"


namespace sibr {

	class Input;
	class Viewport;

	/** Camera navigation matching nerficg's ICGui (src/ICGui/Controls), with its three control schemes:
	 * - Orbital: left drag orbits around a focus point, right drag pans it, mouse wheel zooms.
	 * - Walking: WASD moves in the ground plane, R/Space and F/Shift/Ctrl move along the world up axis,
	 *   left drag looks around, mouse wheel moves forward/backward.
	 * - Flying: like Walking, but all movement is relative to the camera orientation.
	 * Alt/End cycles the schemes; +/-, Page Up/Down change the travel speed.
	 *
	 * Rotations are relative to the pose at the start of a mouse drag (as in ICGui), with yaw around a fixed world
	 * up axis. ICGui uses OpenGL +y as up, which is (0, -1, 0) in the COLMAP-convention world SIBR and nerficg's
	 * (PCA-aligned) datasets use, so that is the default.
	 * \ingroup sibr_view
	 */
	class ICGuiCamera : public ICameraHandler
	{
	public:
		/** The control schemes, in ICGui's cycling order. */
		enum class Scheme : int { ORBITAL = 0, WALKING = 1, FLYING = 2, COUNT = 3 };

		ICGuiCamera();

		/** Take over pose and intrinsics of the given camera. If only the intrinsics changed (same pose), the
		 * scheme's internal state (e.g. the orbit focus point) is kept. */
		void fromCamera(const sibr::InputCamera & cam);

		/** Update the camera from keyboard and mouse input. */
		virtual void update(const sibr::Input & input, const float deltaTime, const Viewport & viewport) override;

		/** \return the current camera */
		virtual const sibr::InputCamera & getCamera(void) const override;

		/** Display the control settings (appended to the ImGui window with the given name). */
		virtual void onGUI(const std::string & suffix) override;

		/** Set the base travel speed in world units per second. ICGui uses 0.05 * the mean edge length of the
		 * dataset's bounding box. */
		void setTravelSpeed(float speed) { _travelSpeed = speed; }

		/** Set the world up axis used for yaw and for vertical movement in the walking scheme. */
		void setWorldUp(const sibr::Vector3f & up);

		/** Switch to another control scheme, keeping the current camera pose. */
		void setScheme(Scheme scheme);
		Scheme scheme() const { return _scheme; }

		/** \return the scheme's display name */
		static const char* schemeName(Scheme scheme);

	private:
		/** Initialize the scheme state from a camera pose (camera position and c2w rotation). As in ICGui, the
		 * orbital focus point is placed in front of the camera at the camera's distance from the world origin. */
		void setPose(const sibr::Vector3f & cameraPosition, const sibr::Quaternionf & rotation);
		/** \return the current camera position */
		sibr::Vector3f cameraPosition() const;
		/** Write the current pose into _camera. */
		void syncCamera();

		void handleKeyboard(const sibr::Input & input, float deltaTime);
		void handleMouse(const sibr::Input & input);
		void move(const sibr::Vector3f & localDirection, float deltaTime);
		void rotate(const sibr::Vector2f & mouseDelta);
		void pan(const sibr::Vector2f & mouseDelta);
		void zoom(float amount);

		sibr::InputCamera _camera; ///< Current camera (intrinsics + synced pose).
		bool _initialized = false;
		Scheme _scheme = Scheme::ORBITAL;
		sibr::Vector3f _worldUp = sibr::Vector3f(0.0f, -1.0f, 0.0f);
		/// Rotation from SIBR's world to ICGui's (OpenGL, +y up) world: 180 degrees around x for the default up axis
		sibr::Quaternionf _toICGuiWorld = sibr::Quaternionf(0.0f, 1.0f, 0.0f, 0.0f);

		// Pose: c2w rotation (OpenGL camera axes, as sibr::Camera) and position. For the orbital scheme, _position
		// is the focus point and the camera sits _distance behind it.
		sibr::Quaternionf _rotation = sibr::Quaternionf::Identity();
		sibr::Vector3f _position = sibr::Vector3f::Zero();
		float _distance = 1.0f;

		// Mouse drag state. ICGui accumulates the mouse delta from the start of a drag and applies it to the pose
		// at that time, instead of applying per-frame increments.
		bool _rotating = false;
		bool _panning = false;
		sibr::Vector2f _dragDelta = sibr::Vector2f::Zero();
		sibr::Quaternionf _startRotation = sibr::Quaternionf::Identity();
		sibr::Vector3f _startPosition = sibr::Vector3f::Zero();
		sibr::Vector3f _pitchAxis = sibr::Vector3f::UnitX();
		float _panningScale = 1.0f;

		// Settings (ICGui defaults)
		float _travelSpeed = 1.0f;
		float _travelSpeedScale = 1.0f;
		float _rotationSpeed = 0.05f;
		float _zoomSpeed = 0.25f;
		float _panningSpeed = 0.0005f;
		bool _invertRotationX = false;
		bool _invertRotationY = false;
		bool _invertPanningX = false;
		bool _invertPanningY = false;
	};

}
