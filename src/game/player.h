#pragma once

#include "data/entity.h"
#include "lmath.h"
#include "physics.h"
#include "render/ui.h"
#include "ai.h"
#include "menu.h"
#include "team.h"

namespace VI
{

struct RigidBody;
struct Transform;
struct LocalPlayerControl;
struct PlayerManager;
struct Health;

#define MAX_PLAYER_COMBOS 28 // C(MAX_PLAYERS, 2)

struct LocalPlayer
{
	enum class UIMode { Default, Pause, Spawning, AbilityMenu };

	enum class AbilityMenu { None, Select, Upgrade };

	static PinArray<LocalPlayer, MAX_PLAYERS> list;

	u8 gamepad;
	b8 pause;
	AbilityMenu ability_menu;
	UIMenu menu;
	Ref<Transform> map_view;
	Ref<PlayerManager> manager;
	Camera* camera;
	r32 msg_timer;
	UIText msg_text;
	UIText credits_text;
	Revision revision;
	b8 options_menu;

	inline ID id() const
	{
		return this - &list[0];
	}

	LocalPlayer(PlayerManager*, u8);

	void msg(const char*);
	UIMode ui_mode() const;
	void update(const Update&);
	void draw_alpha(const RenderParams&) const;
	void ensure_camera(const Update&, b8);
	void spawn();
};

struct PlayerCommon : public ComponentType<PlayerCommon>
{
	r32 cooldown;
	r32 cooldown_multiplier;
	UIText username_text;
	s32 visibility_index;
	Ref<PlayerManager> manager;

	r32 detect_danger() const;

	PlayerCommon(PlayerManager*);
	void awake();
	void update(const Update&);
};

struct LocalPlayerControl : public ComponentType<LocalPlayerControl>
{
	enum class TraceType
	{
		None,
		Normal,
		Target,
	};

	struct TraceEntry
	{
		TraceType type;
		Vec3 pos;
	};

	static LocalPlayerControl* player_for_camera(const Camera*);

	Ref<LocalPlayer> player;

	TraceEntry tracer;
	r32 angle_horizontal;
	r32 angle_vertical;
	Quat attach_quat;
	Camera* camera;
	r32 fov_blend;
	r32 lean;
	r32 last_angle_horizontal;
	b8 allow_zoom;
	b8 try_parkour;
	b8 try_jump;
	b8 try_slide;
	u8 gamepad;
	b8 enable_input;

	LocalPlayerControl(u8);
	~LocalPlayerControl();
	void awake();

	void awk_bounce(const Vec3&);
	void awk_attached();
	void hit_target(Entity*);

	void clamp_rotation(const Vec3&);

	void update(const Update&);
	void draw_alpha(const RenderParams&) const;
	Vec3 look_dir() const;

	void detach(const Vec3&);

	void update_camera_input(const Update&);
	Vec3 get_movement(const Update&, const Quat&);
	b8 input_enabled() const;
};

}