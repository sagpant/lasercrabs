#pragma once

#include "data/entity.h"
#include "lmath.h"
#include "physics.h"
#include "render/ui.h"
#include "ai.h"
#include "game.h"

namespace VI
{


struct Camera;
struct Transform;
struct Target;
struct TargetEvent;
struct PlayerManager;
struct SpawnPosition;
struct SpawnPoint;
namespace Net
{
	struct StreamRead;
}

struct AbilityInfo
{
	enum class Type : s8
	{
		Build,
		Shoot,
		Other,
		Passive,
		count,
	};

	static AbilityInfo list[s32(Ability::count) + 1]; // +1 for Ability::None

	r32 cooldown_movement;
	r32 cooldown_switch;
	r32 cooldown_use;
	r32 cooldown_use_threshold;
	r32 recoil_velocity;
	AkUniqueID equip_sound;
	AssetID icon;
	Type type;
};

struct UpgradeInfo
{
	enum class Type : s8
	{
		Ability,
		Consumable,
		count,
	};

	static UpgradeInfo list[s32(Upgrade::count)];

	AssetID name;
	AssetID description;
	AssetID icon;
	s16 cost;
	Type type;
};

#define PLAYER_SCORE_SUMMARY_ITEMS 4

struct Team : public ComponentType<Team>
{
	enum class MatchState : s8
	{
		Waiting,
		TeamSelect,
		Active,
		Done,
		count,
	};

	struct ScoreSummaryItem
	{
		s32 amount;
		Ref<PlayerManager> player;
		AssetID icon;
		AI::Team team;
		char label[512];
	};

	static const Vec4& color_neutral();
	static const Vec4& color_friend();
	static const Vec4& color_enemy();
	static const Vec4& color_ui_friend();
	static const Vec4& color_ui_enemy();
	static const Vec3& color_alpha_friend();
	static const Vec3& color_alpha_enemy();
	static StaticArray<ScoreSummaryItem, MAX_PLAYERS * PLAYER_SCORE_SUMMARY_ITEMS> score_summary;
	static r32 game_over_real_time;
	static r32 transition_timer;
	static r32 match_time;
	static r32 battery_spawn_delay;
	static Ref<Team> winner;
	static MatchState match_state;
	static b8 parkour_game_start_impending();

	static void awake_all();
	static void transition_next();
	static void battery_spawn();
	static s16 force_field_mask(AI::Team);
	static void update_all(const Update&);
	static void update_all_server(const Update&);
	static void update_all_client_only(const Update&);
	static s32 teams_with_active_players();
	static Team* with_most_kills();
	static Team* with_most_flags();
	static Team* with_most_energy_collected();
	static Team* with_least_players(s32* = nullptr);
	static b8 net_msg(Net::StreamRead*, Net::MessageSource);
	static void draw_ui(const RenderParams&);
	static void match_start();
	static void match_team_select();
	static void match_waiting();
	static AssetID name_selector(AI::Team);
	static AssetID name_long(AI::Team);

	static inline const Vec4& color_ui(AI::Team me, AI::Team them)
	{
		return them == AI::TeamNone ? UI::color_accent() : (me == them ? color_ui_friend() : color_ui_enemy());
	}

	static inline const Vec4& color(AI::Team me, AI::Team them)
	{
		return me == them ? color_friend() : color_enemy();
	}

	static inline const Vec3& color_alpha(AI::Team me, AI::Team them)
	{
		return me == them ? color_alpha_friend() : color_alpha_enemy();
	}

	Ref<Target> spot_target;
	Ref<Transform> flag_base;
	s16 kills;
	s16 flags_captured;
	s16 energy_collected;

	void awake() {}
	b8 has_active_player() const;
	s32 player_count() const;
	void add_kills(s32);
	SpawnPoint* get_spawn_point() const;

	inline AI::Team team() const
	{
		return AI::Team(id());
	}
};

struct PlayerManager : public ComponentType<PlayerManager>
{
	enum State : s8
	{
		Default,
		Upgrading,
		count,
	};

	enum class Message : s8
	{
		CanSpawn,
		ScoreAccept,
		UpgradeFailed,
		UpgradeCompleted,
		UpdateCounts,
		MakeAdmin,
		MakeOtherAdmin,
		Kick,
		Ban,
		TeamSchedule,
		TeamSwitch,
		MapSchedule,
		MapSkip,
		Chat,
		Leave,
		Spot,
		AbilityCooldownReady,
		ParkourReady,
		count,
	};

	struct Visibility
	{
		b8 value;
	};

	enum Flags : s8
	{
		FlagScoreAccepted = 1 << 0,
		FlagCanSpawn = 1 << 1,
		FlagIsAdmin = 1 << 2,
		FlagIsVip = 1 << 3,
		FlagParkourReady = 1 << 4,
	};

	static s32 visibility_hash(const PlayerManager*, const PlayerManager*);
	static Visibility visibility[MAX_PLAYERS * MAX_PLAYERS];

	static void update_all(const Update&);
	static b8 net_msg(Net::StreamRead*, PlayerManager*, Message, Net::MessageSource);
	static PlayerManager* owner(const Entity*);
	static void entity_killed_by(Entity*, Entity*);
	static s32 count_parkour_ready();
	static s32 count_team_mask(AI::TeamMask);

	r32 spawn_timer;
	r32 state_timer;
	r32 ability_cooldown[s32(Ability::count) + 1];
	r32 ability_flash_time[MAX_ABILITIES];
	LinkArg<const SpawnPosition&> spawn;
	LinkArg<Upgrade> upgrade_completed;
	Ref<Team> team;
	Ref<Entity> instance;
	s16 energy;
	s16 energy_collected;
	s16 kills;
	s16 deaths;
	s16 flags_captured;
	s16 upgrades;
	char username[MAX_USERNAME + 1]; // +1 for null terminator
	Ability abilities[MAX_ABILITIES];
	Upgrade current_upgrade;
	AI::Team team_scheduled;
	s8 current_upgrade_ability_slot;
	s8 flags;

	inline void flag(s32 f, b8 value)
	{
		if (value)
			flags |= f;
		else
			flags &= ~f;
	}
	
	inline b8 flag(s32 f)
	{
		return flags & f;
	}

	PlayerManager(Team* = nullptr, const char* = nullptr);
	void awake();
	~PlayerManager();

	void make_admin(b8 = true);
	void make_admin(PlayerManager*, b8 = true);
	void clear_ownership();
	State state() const;
	b8 can_transition_state() const;
	b8 has_upgrade(Upgrade) const;
	b8 has_ability(Ability) const;
	b8 is_local() const;
	s32 ability_count() const;
	b8 ability_valid(Ability) const;
	b8 upgrade_start(Upgrade, s8 = 0);
	Upgrade upgrade_highest_owned_or_available() const;
	b8 upgrade_available(Upgrade = Upgrade::None) const;
	s16 upgrade_cost(Upgrade) const;
	void kick(PlayerManager*);
	void ban(PlayerManager*);
	void ability_cooldown_apply(Ability);
	void kick();
	void leave();
	void add_energy(s32);
	void add_energy_and_notify(s32);
	void add_kills(s32);
	void add_deaths(s32);
	void captured_flag();
	void update_server(const Update&);
	void update_client_only(const Update&);
	void score_accept();
	void set_can_spawn(b8 = true);
	void team_schedule(AI::Team);
	void chat(const char*, AI::TeamMask);
	void spot(Target*);
	void map_schedule(AssetID);
	void map_skip(AssetID);
	void parkour_ready(b8);
};


}
