#include "ICGuiCamera.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "core/graphics/GUI.hpp"
#include "core/graphics/Input.hpp"
#include "core/graphics/Viewport.hpp"
#include "core/view/UIShortcuts.hpp"


namespace sibr {

	namespace {
		// Camera-local axes (OpenGL convention, as sibr::Camera and ICGui)
		const Vector3f LOCAL_RIGHT(1.0f, 0.0f, 0.0f);
		const Vector3f LOCAL_UP(0.0f, 1.0f, 0.0f);
		const Vector3f LOCAL_BACKWARD(0.0f, 0.0f, 1.0f);

		constexpr float PI = std::numbers::pi_v<float>;

		/** Rotation of q around the (unit) axis d, i.e. the twist of a swing-twist decomposition. */
		Quaternionf twistAround(const Quaternionf& q, const Vector3f& d) {
			const Vector3f projection = q.vec().dot(d) * d;
			Quaternionf twist(q.w(), projection.x(), projection.y(), projection.z());
			if (twist.squaredNorm() < 1e-12f) {
				return Quaternionf::Identity();  // Rotation by 180 degrees around an axis perpendicular to d
			}
			return twist.normalized();
		}
	}

	ICGuiCamera::ICGuiCamera() {
		UIShortcuts::global().add("[ICGui camera] alt / end", "cycle controls (orbital, walking, flying)");
		UIShortcuts::global().add("[ICGui camera] left drag", "orbit (orbital) / look around");
		UIShortcuts::global().add("[ICGui camera] right drag", "pan (orbital)");
		UIShortcuts::global().add("[ICGui camera] wheel", "zoom (orbital) / move forward or backward");
		UIShortcuts::global().add("[ICGui camera] w a s d", "move (walking, flying)");
		UIShortcuts::global().add("[ICGui camera] r / space", "move up (walking, flying)");
		UIShortcuts::global().add("[ICGui camera] f / shift / ctrl", "move down (walking, flying)");
		UIShortcuts::global().add("[ICGui camera] + / - / page up / page down", "change travel speed");
	}

	const char* ICGuiCamera::schemeName(Scheme scheme) {
		switch (scheme) {
			case Scheme::ORBITAL: return "Orbital";
			case Scheme::WALKING: return "Walking";
			case Scheme::FLYING:  return "Flying";
			default:              return "Unknown";
		}
	}

	void ICGuiCamera::setWorldUp(const Vector3f& up) {
		if (up.squaredNorm() <= 1e-12f) {
			return;
		}
		_worldUp = up.normalized();
		if (_worldUp.dot(Vector3f(0.0f, -1.0f, 0.0f)) > 1.0f - 1e-6f) {
			// COLMAP -> OpenGL world, as ICGui: 180 degrees around x (FromTwoVectors would pick an arbitrary axis)
			_toICGuiWorld = Quaternionf(0.0f, 1.0f, 0.0f, 0.0f);
		} else {
			_toICGuiWorld = Quaternionf::FromTwoVectors(_worldUp, Vector3f::UnitY());
		}
	}

	void ICGuiCamera::fromCamera(const InputCamera& cam) {
		const bool samePose = _initialized
			&& (cam.position() - _camera.position()).norm() <= 1e-6f * std::max(1.0f, _camera.position().norm())
			&& std::abs(cam.rotation().dot(_camera.rotation())) >= 1.0f - 1e-7f;
		_camera = cam;
		if (!samePose) {
			setPose(cam.position(), cam.rotation());
			_rotating = _panning = false;
		}
		_initialized = true;
		syncCamera();
	}

	void ICGuiCamera::setScheme(Scheme scheme) {
		const int index = static_cast<int>(scheme) % static_cast<int>(Scheme::COUNT);
		const Scheme newScheme = static_cast<Scheme>(index < 0 ? index + static_cast<int>(Scheme::COUNT) : index);
		if (newScheme == _scheme) {
			return;
		}
		// Like ICGui, re-create the scheme state from the current camera (this resets the orbit focus point)
		const Vector3f position = cameraPosition();
		_scheme = newScheme;
		setPose(position, _rotation);
		_rotating = _panning = false;
		syncCamera();
		SIBR_LOG << "ICGui camera controls: " << schemeName(_scheme) << std::endl;
	}

	void ICGuiCamera::setPose(const Vector3f& position, const Quaternionf& rotation) {
		_rotation = rotation.normalized();
		if (_scheme == Scheme::ORBITAL) {
			_distance = position.norm();
			_position = position - _distance * (_rotation * LOCAL_BACKWARD);
		} else {
			_position = position;
		}
	}

	Vector3f ICGuiCamera::cameraPosition() const {
		if (_scheme == Scheme::ORBITAL) {
			return _position + _distance * (_rotation * LOCAL_BACKWARD);
		}
		return _position;
	}

	void ICGuiCamera::syncCamera() {
		_camera.rotation(_rotation);
		_camera.position(cameraPosition());
	}

	const InputCamera& ICGuiCamera::getCamera() const {
		if (!_initialized) {
			SIBR_ERR << "ICGui camera: camera not initialized before use (call fromCamera())" << std::endl;
		}
		return _camera;
	}

	void ICGuiCamera::update(const Input& input, const float deltaTime, const Viewport& viewport) {
		if (!_initialized) {
			return;
		}
		handleKeyboard(input, deltaTime);
		handleMouse(input);
		syncCamera();
	}

	void ICGuiCamera::handleKeyboard(const Input& input, float deltaTime) {
		const auto& key = input.key();

		if (key.isPressed(Key::LeftAlt) || key.isPressed(Key::RightAlt) || key.isPressed(Key::End)) {
			setScheme(static_cast<Scheme>(static_cast<int>(_scheme) + 1));
		}
		if (key.isPressed(Key::KPAdd) || key.isPressed(Key::Equal) || key.isPressed(Key::Page_up)) {
			_travelSpeedScale *= 1.1f;
		}
		if (key.isPressed(Key::KPSubtract) || key.isPressed(Key::Minus) || key.isPressed(Key::Page_down)) {
			_travelSpeedScale *= 0.9f;
		}

		// The orbital scheme has no keyboard movement in ICGui
		if (_scheme == Scheme::ORBITAL) {
			return;
		}

		// ICGui handles each action once per frame, even if several of its keys are held
		const bool forward = key.isActivated(Key::W);
		const bool backward = key.isActivated(Key::S);
		const bool left = key.isActivated(Key::A);
		const bool right = key.isActivated(Key::D);
		const bool up = key.isActivated(Key::R) || key.isActivated(Key::Space);
		const bool down = key.isActivated(Key::F) || key.isActivated(Key::LeftShift) || key.isActivated(Key::RightShift)
			|| key.isActivated(Key::LeftControl) || key.isActivated(Key::RightControl);

		if (forward)  move(-LOCAL_BACKWARD, deltaTime);
		if (backward) move(LOCAL_BACKWARD, deltaTime);
		if (left)     move(-LOCAL_RIGHT, deltaTime);
		if (right)    move(LOCAL_RIGHT, deltaTime);
		if (up)       move(LOCAL_UP, deltaTime);
		if (down)     move(-LOCAL_UP, deltaTime);
	}

	void ICGuiCamera::move(const Vector3f& localDirection, float deltaTime) {
		Vector3f direction;
		if (_scheme == Scheme::WALKING) {
			// Horizontal movement follows the heading only, vertical movement is along the world up axis.
			// The heading (twist around up) has to be taken in ICGui's frame, where the unrotated camera is upright;
			// in SIBR's y-down world it is upside down, which would flip forward and backward.
			const Quaternionf rotationICGui = _toICGuiWorld * _rotation;
			const Quaternionf heading = twistAround(rotationICGui, Vector3f::UnitY());
			const Vector3f horizontal = heading * Vector3f(localDirection.x(), 0.0f, localDirection.z());
			direction = _toICGuiWorld.conjugate() * horizontal + localDirection.y() * _worldUp;
		} else {
			direction = _rotation * localDirection;
		}
		_position += _travelSpeed * _travelSpeedScale * deltaTime * direction;
	}

	void ICGuiCamera::handleMouse(const Input& input) {
		const auto& mouse = input.mouseButton();
		// Mouse movement in window pixels, x right / y down (SIBR's mouseDeltaPosition() is previous - current)
		const Vector2f frameDelta = (-input.mouseDeltaPosition()).cast<float>();

		// Rotation (left button). A drag ends when the button is released or the cursor leaves the view, since
		// SIBR clears the buttons outside of it (ICGui captures the mouse instead).
		if (_rotating && !mouse.isActivated(Mouse::Left)) {
			_rotating = false;
		}
		if (!_rotating && !_panning && mouse.isPressed(Mouse::Left)) {
			_rotating = true;
			_dragDelta.setZero();
			_startRotation = _rotation;
			_pitchAxis = _startRotation * LOCAL_RIGHT;  // Orbital: local x axis at the start of the drag
		} else if (_rotating) {
			_dragDelta += frameDelta;
			rotate(_dragDelta);
		}

		// Panning (right button, orbital only)
		if (_panning && !mouse.isActivated(Mouse::Right)) {
			_panning = false;
		}
		if (_scheme == Scheme::ORBITAL && !_rotating && !_panning && mouse.isPressed(Mouse::Right)) {
			_panning = true;
			_dragDelta.setZero();
			_startPosition = _position;
			// Proportional to the orbit distance, so independent of the scene scale
			_panningScale = _panningSpeed * _travelSpeedScale * _distance;
		} else if (_panning) {
			_dragDelta += frameDelta;
			pan(_dragDelta);
		}

		if (input.mouseScroll() != 0.0) {
			zoom(static_cast<float>(input.mouseScroll()));
		}
	}

	void ICGuiCamera::rotate(const Vector2f& mouseDelta) {
		// ICGui uses 1 degree per unit for flying / walking and 2 for orbital
		const float radiansPerUnit = _rotationSpeed * PI / (_scheme == Scheme::ORBITAL ? 90.0f : 180.0f);
		const float yaw = (_invertRotationX ? 1.0f : -1.0f) * mouseDelta.x() * radiansPerUnit;
		const float pitch = (_invertRotationY ? 1.0f : -1.0f) * mouseDelta.y() * radiansPerUnit;
		const Quaternionf yawRotation(Eigen::AngleAxisf(yaw, _worldUp));

		if (_scheme == Scheme::ORBITAL) {
			// Pitch around the local x axis at the start of the drag, applied before yaw
			const Quaternionf pitchRotation(Eigen::AngleAxisf(pitch, _pitchAxis));
			_rotation = (yawRotation * pitchRotation * _startRotation).normalized();
		} else {
			// Pitch around the local x axis after the yaw, which avoids instabilities near the poles
			const Vector3f pitchAxis = (yawRotation * _startRotation) * LOCAL_RIGHT;
			const Quaternionf pitchRotation(Eigen::AngleAxisf(pitch, pitchAxis));
			_rotation = (pitchRotation * yawRotation * _startRotation).normalized();
		}
	}

	void ICGuiCamera::pan(const Vector2f& mouseDelta) {
		const float xScale = _invertPanningX ? -1.0f : 1.0f;
		const float yScale = _invertPanningY ? -1.0f : 1.0f;
		_position = _startPosition
			+ xScale * _panningScale * -mouseDelta.x() * (_rotation * LOCAL_RIGHT)
			+ yScale * _panningScale * mouseDelta.y() * (_rotation * LOCAL_UP);
	}

	void ICGuiCamera::zoom(float amount) {
		if (_scheme == Scheme::ORBITAL) {
			// Always move some minimum distance, and never zoom in too far
			_distance = std::max(_distance - amount * std::max(_distance * _zoomSpeed * 0.5f, _zoomSpeed / 20.0f), 0.01f);
		} else {
			_position -= _zoomSpeed * amount * (_rotation * LOCAL_BACKWARD);
		}
	}

	void ICGuiCamera::onGUI(const std::string& suffix) {
		if (ImGui::Begin(suffix.c_str())) {
			ImGui::Text("ICGui Controls");
			int scheme = static_cast<int>(_scheme);
			if (ImGui::Combo("Controls", &scheme, "Orbital\0Walking\0Flying\0\0")) {
				setScheme(static_cast<Scheme>(scheme));
			}
			ImGui::DragFloat("Travel Speed", &_travelSpeed, std::max(_travelSpeed * 0.01f, 1e-4f), 0.0f, 1e6f, "%.3f");
			ImGui::DragFloat("Speed Scale", &_travelSpeedScale, 0.01f, 0.001f, 1000.0f, "%.3f");
			ImGui::DragFloat("Rotation Speed", &_rotationSpeed, 0.001f, 0.001f, 1.0f, "%.3f");
			ImGui::DragFloat("Zoom Speed", &_zoomSpeed, 0.01f, 0.001f, 100.0f, "%.3f");
			ImGui::Checkbox("Invert Rotation X", &_invertRotationX);
			ImGui::SameLine();
			ImGui::Checkbox("Invert Rotation Y", &_invertRotationY);
			if (_scheme == Scheme::ORBITAL) {
				ImGui::Checkbox("Invert Panning X", &_invertPanningX);
				ImGui::SameLine();
				ImGui::Checkbox("Invert Panning Y", &_invertPanningY);
				ImGui::TextWrapped("Left drag: orbit, right drag: pan, wheel: zoom, Alt/End: next controls");
			} else {
				ImGui::TextWrapped("WASD: move, R/Space: up, F/Shift/Ctrl: down, left drag: look, wheel: forward/back, "
								   "+/-: speed, Alt/End: next controls");
			}
		}
		ImGui::End();
	}

}
