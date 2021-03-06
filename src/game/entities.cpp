#include "localization.h"
#include "entities.h"
#include "data/animator.h"
#include "data/components.h"
#include "render/skinned_model.h"
#include "walker.h"
#include "asset/armature.h"
#include "asset/animation.h"
#include "asset/shader.h"
#include "asset/texture.h"
#include "asset/mesh.h"
#include "recast/Detour/Include/DetourNavMeshQuery.h"
#include "mersenne/mersenne-twister.h"
#include "game.h"
#include "audio.h"
#include "asset/Wwise_IDs.h"
#include "render/views.h"
#include "drone.h"
#include "bullet/src/BulletCollision/NarrowPhaseCollision/btRaycastCallback.h"
#include "menu.h"
#include "data/ragdoll.h"
#include "console.h"
#include "minion.h"
#include "render/particles.h"
#include "data/priority_queue.h"
#include "net.h"
#include "team.h"
#include "parkour.h"
#include "overworld.h"
#include "common.h"
#include "player.h"
#include "load.h"
#include "ease.h"

#define TURRET_TARGET_CHECK_TIME 0.75f

namespace VI
{


void spawn_sparks(const Vec3& pos, const Quat& rot, Transform* parent)
{
	for (s32 i = 0; i < 15; i++)
	{
		Particles::sparks.add
		(
			pos,
			rot * Vec3(mersenne::randf_oo() * 2.0f - 1.0f, mersenne::randf_oo() * 2.0f - 1.0f, mersenne::randf_oo()) * 10.0f,
			Vec4(1, 1, 1, 1)
		);
	}

	EffectLight::add(pos, 3.0f, 0.25f, EffectLight::Type::Spark, parent);
}

DroneEntity::DroneEntity(AI::Team team, const Vec3& pos)
{
	create<Audio>();
	{
		Transform* transform = create<Transform>();
		transform->pos = pos;
		transform->rot = Quat::look(Vec3(0, -1, 0));
	}
	create<Drone>();
	create<AIAgent>()->team = team;
	{
		Health* health = create<Health>(DRONE_HEALTH, DRONE_HEALTH, Game::session.config.ruleset.drone_shield, Game::session.config.ruleset.drone_shield);
		health->active_armor_timer = GRENADE_DELAY + 0.5f; // drones are invincible while spawning
	}
	create<Shield>();

	SkinnedModel* model = create<SkinnedModel>();
	model->mesh = Asset::Mesh::drone;
	model->shader = Asset::Shader::armature;
	model->team = s8(team);
	model->color = Team::color_neutral();

	Animator* anim = create<Animator>();
	anim->armature = Asset::Armature::drone;

	create<RigidBody>(RigidBody::Type::Sphere, Vec3(DRONE_RADIUS * 0.75f), 0.0f, CollisionDroneIgnore, ~DRONE_PERMEABLE_MASK & ~get<Drone>()->ally_force_field_mask());

	create<Target>();
}

Health::Health(s8 hp, s8 hp_max, s8 shield, s8 shield_max)
	: hp(hp),
	hp_max(hp_max),
	shield(shield),
	shield_max(shield_max),
	changed(),
	killed(),
	regen_timer()
{
}

template<typename Stream> b8 serialize_health_event(Stream* p, Health* h, HealthEvent* e)
{
	serialize_ref(p, e->source);
	if (h->hp_max > 0)
		serialize_int(p, s8, e->hp, -h->hp_max, h->hp_max);
	else if (Stream::IsReading)
		e->hp = 0;
	if (h->shield_max > 0)
		serialize_int(p, s8, e->shield, -h->shield_max, h->shield_max);
	else if (Stream::IsReading)
		e->shield = 0;
	return true;
}

b8 Health::net_msg(Net::StreamRead* p)
{
	using Stream = Net::StreamRead;
	Ref<Health> ref;
	serialize_ref(p, ref);

	HealthEvent e;
	if (!serialize_health_event(p, ref.ref(), &e))
		net_error();

	Health* h = ref.ref();
	h->hp += e.hp;
	h->shield += e.shield;
	h->changed.fire(e);
	if (e.hp < 0 && h->hp == 0)
		h->killed.fire(e.source.ref());

	return true;
}

// only called on server
b8 health_send_event(Health* h, HealthEvent* e)
{
	using Stream = Net::StreamWrite;
	Net::StreamWrite* p = Net::msg_new(Net::MessageType::Health);

	Ref<Health> ref = h;
	serialize_ref(p, ref);

	if (!serialize_health_event(p, ref.ref(), e))
		net_error();

	Net::msg_finalize(p);

	return true;
}

void health_internal_apply_damage(Health* h, Entity* e, s8 damage)
{
	vi_assert(Game::level.local);

	s8 shield_value = h->shield;

	s8 damage_accumulator = damage;
	s8 damage_shield;
	if (damage_accumulator > shield_value)
	{
		damage_shield = shield_value;
		damage_accumulator -= shield_value;
	}
	else
	{
		damage_shield = damage_accumulator;
		damage_accumulator = 0;
	}

	s8 damage_hp;
	if (damage_accumulator > h->hp)
		damage_hp = h->hp;
	else
		damage_hp = damage_accumulator;

	if (damage_hp != 0 || damage_shield != 0)
	{
		h->regen_timer = SHIELD_REGEN_TIME + SHIELD_REGEN_DELAY;

		HealthEvent ev =
		{
			e,
			s8(-damage_hp),
			s8(-damage_shield),
		};
		health_send_event(h, &ev);
	}
}

void Health::update(const Update& u)
{
	if (Game::level.local)
	{
		// regen
		if (shield < shield_max)
		{
			r32 old_timer = regen_timer;
			regen_timer -= u.time.delta;
			if (regen_timer < SHIELD_REGEN_TIME)
			{
				const r32 regen_interval = SHIELD_REGEN_TIME / r32(shield_max);
				if (s32(old_timer / regen_interval) != s32(regen_timer / regen_interval))
				{
					HealthEvent e =
					{
						nullptr,
						0,
						1,
					};
					health_send_event(this, &e);
				}
			}
		}

		// damage buffering
		for (s32 i = 0; i < damage_buffer.length; i++)
		{
			BufferedDamage* entry = &damage_buffer[i];
			entry->delay -= u.time.delta;
			if (entry->delay < 0.0f) // IT'S TIME
			{
				Entity* src = entry->source.ref();
				if (src)
				{
					if (src->has<Bolt>())
					{
						if (active_armor())
							src->get<Bolt>()->reflect(entity());
						else
						{
							health_internal_apply_damage(this, src, entry->damage);
							World::remove_deferred(src);
						}
					}
					else if (active_armor() && src->has<Drone>() && entry->type != BufferedDamage::Type::Sniper) // damage them back
						src->get<Health>()->damage_force(entity(), DRONE_HEALTH + Game::session.config.ruleset.drone_shield);
					else
						health_internal_apply_damage(this, src, entry->damage);
				}
				damage_buffer.remove(i);
				i--;
			}
		}
	}

	active_armor_timer = vi_max(0.0f, active_armor_timer - u.time.delta);
}

b8 Health::damage_buffer_required(const Entity* src) const
{
#if SERVER
	return has<PlayerControlHuman>()
		&& !get<PlayerControlHuman>()->local() // we are a remote player
		&& get<PlayerCommon>()->manager.ref()->has_ability(Ability::ActiveArmor)
		&& src
		&& (!src->has<PlayerControlHuman>() || !PlayerHuman::players_on_same_client(entity(), src)); // the attacker is remote from the player
#else
	return false;
#endif
}

void Health::damage(Entity* src, s8 damage, const Net::StateFrame* state_frame)
{
	vi_assert(Game::level.local);
	vi_assert(can_take_damage(src, state_frame));
	if (hp > 0 && damage > 0)
	{
		if (damage_buffer_required(src))
		{
			// do damage buffering
			BufferedDamage entry;
			entry.source = src;
			entry.damage = damage;
			entry.delay = (vi_min(NET_MAX_RTT_COMPENSATION, Net::rtt(get<PlayerControlHuman>()->player.ref())) + Net::interpolation_delay(get<PlayerControlHuman>()->player.ref()) + Net::tick_rate()) * Game::session.effective_time_scale();
			if (src->has<Drone>() && src->get<Drone>()->current_ability == Ability::Sniper)
				entry.type = BufferedDamage::Type::Sniper;
			else
				entry.type = BufferedDamage::Type::Other;
			damage_buffer.add(entry);
		}
		else // apply damage immediately
			health_internal_apply_damage(this, src, damage);
	}
}

void Health::damage_force(Entity* src, s8 damage)
{
	vi_assert(Game::level.local);
	if (hp > 0 && damage > 0)
		health_internal_apply_damage(this, src, damage);
}

void Health::reset_hp()
{
	if (hp < hp_max)
		add(hp_max - hp);
}

// bypasses all invincibility calculations and damage buffering
void Health::kill(Entity* e)
{
	damage_force(e, hp + shield);
}

void Health::add(s8 amount)
{
	vi_assert(Game::level.local);
	amount = vi_min(amount, s8(hp_max - hp));
	if (amount > 0)
	{
		HealthEvent e =
		{
			nullptr,
			amount,
			0,
		};
		health_send_event(this, &e);
	}
}

s8 Health::total() const
{
	return hp + shield;
}

b8 Health::active_armor(const Net::StateFrame* state_frame) const
{
	if (has<ForceField>())
		return active_armor_timer > 0.0f || (get<ForceField>()->flags & ForceField::FlagInvincible);
	else if (has<Drone>())
	{
		if (state_frame && state_frame->drones[get<Drone>()->id()].active)
			return state_frame->drones[get<Drone>()->id()].collision_state == DroneCollisionState::ActiveArmor;
		else
			return active_armor_timer > 0.0f;
	}
	else
		return active_armor_timer > 0.0f;
}

b8 Health::can_take_damage(Entity* damager, const Net::StateFrame* state_frame) const
{
	if (active_armor(state_frame))
		return false;

	if (has<Drone>())
	{
		DroneCollisionState collision_state;
		if (state_frame
			&& state_frame->drones[get<Drone>()->id()].active
			&& !PlayerHuman::players_on_same_client(entity(), damager))
			collision_state = state_frame->drones[get<Drone>()->id()].collision_state;
		else
			collision_state = get<Drone>()->collision_state();
		switch (collision_state)
		{
			case DroneCollisionState::Default:
				return true;
			case DroneCollisionState::UpgradeStation:
			case DroneCollisionState::ActiveArmor:
				return false;
			case DroneCollisionState::FlyingDashing:
			{
				// grenades, bolts, and snipers can still damage drones while they are flying or dashing
				if (damager)
				{
					if (damager->has<Grenade>())
						return true;
					else if (damager->has<Bolt>())
					{
						Bolt::Type t = damager->get<Bolt>()->type;
						return t == Bolt::Type::DroneBolter || t == Bolt::Type::DroneShotgun;
					}
					else if (damager->has<Drone>())
						return damager->get<Drone>()->current_ability == Ability::Sniper;
				}
				return false;
			}
			default:
			{
				vi_assert(false);
				return false;
			}
		}
	}
	else if (has<Battery>() && damager && damager->has<Bolt>())
		return get<Battery>()->team != damager->get<Bolt>()->team;
	else
		return true;
}

#define DRONE_SHIELD_ALPHA 0.3f
#define DRONE_SHIELD_VIEW_RATIO 0.6f
#define DRONE_OVERSHIELD_ALPHA 0.45f

void Shield::awake()
{
	if (Game::level.local && !inner.ref() && get<Health>()->shield_max > 0)
	{
		AI::Team team;
		if (has<MinionSpawner>())
			team = get<MinionSpawner>()->team;
		else if (has<Rectifier>())
			team = get<Rectifier>()->team;
		else if (has<Turret>())
			team = get<Turret>()->team;
		else
			team = get<AIAgent>()->team;

		{
			// active armor
			vi_assert(!active_armor.ref());
			Entity* shield_entity = World::create<Empty>();
			shield_entity->get<Transform>()->parent = get<Transform>();

			View* s = shield_entity->add<View>();
			s->mesh = Asset::Mesh::sphere_highres;
			s->shader = Asset::Shader::fresnel;
			s->color = Vec4(1, 1, 1, 0);
			s->offset.scale(Vec3(DRONE_OVERSHIELD_RADIUS * DRONE_SHIELD_VIEW_RATIO * 1.1f));
			s->alpha();
			active_armor = s;

			Net::finalize_child(shield_entity);
		}

		{
			// inner shield
			Entity* shield_entity = World::create<Empty>();
			shield_entity->get<Transform>()->parent = get<Transform>();

			View* s = shield_entity->add<View>();
			s->team = s8(team);
			s->mesh = Asset::Mesh::sphere_highres;
			s->shader = Asset::Shader::fresnel;
			s->color.w = 0.0f;
			s->offset.scale(Vec3::zero);
			s->alpha();
			inner = s;

			Net::finalize_child(shield_entity);
		}

		{
			// outer shield
			vi_assert(!outer.ref());
			Entity* shield_entity = World::create<Empty>();
			shield_entity->get<Transform>()->parent = get<Transform>();

			View* s = shield_entity->add<View>();
			s->team = s8(team);
			s->mesh = Asset::Mesh::sphere_highres;
			s->shader = Asset::Shader::fresnel;
			s->color.w = 0.0f;
			s->offset.scale(Vec3::zero);
			s->alpha();
			outer = s;

			Net::finalize_child(shield_entity);
		}
	}

	link_arg<const HealthEvent&, &Shield::health_changed>(get<Health>()->changed);
}

// not synced over network
Shield::~Shield()
{
	if (Game::level.local)
	{
		if (inner.ref())
			World::remove_deferred(inner.ref()->entity());
		if (outer.ref())
			World::remove_deferred(outer.ref()->entity());
		if (active_armor.ref())
			World::remove_deferred(active_armor.ref()->entity());
	}
}

void Shield::update_all(const Update& u)
{
	static r32 particle_accumulator = 0.0f;
	static r32 particle_interval = 0.05f;
	particle_accumulator += u.time.delta;

	s32 particles = s32(particle_accumulator / particle_interval);
	if (particles > 0)
	{
		particle_accumulator -= particle_interval * particles;
		particle_interval = 0.01f + mersenne::randf_cc() * 0.005f;

		for (auto i = list.iterator(); !i.is_last(); i.next())
		{
			if (i.item()->get<Health>()->active_armor())
			{
				Vec3 pos = i.item()->get<Transform>()->absolute_pos();
				for (s32 j = 0; j < particles; j++)
				{
					s32 cluster = 1 + s32(mersenne::randf_co() * 3.0f);
					Vec3 cluster_center = pos + Quat::euler(0.0f, mersenne::randf_co() * PI * 2.0f, (mersenne::randf_co() - 0.5f) * PI) * Vec3(0, 0, DRONE_SHIELD_RADIUS * 1.1f);
					for (s32 k = 0; k < cluster; k++)
					{
						Particles::sparkles.add
						(
							cluster_center + Vec3((mersenne::randf_co() - 0.5f) * 0.2f, (mersenne::randf_co() - 0.5f) * 0.2f, (mersenne::randf_co() - 0.5f) * 0.2f),
							Vec3::zero,
							mersenne::randf_co() * PI * 2.0f
						);
					}
				}
			}
		}
	}

	for (auto i = list.iterator(); !i.is_last(); i.next())
		i.item()->update_client(u);
}

void Shield::health_changed(const HealthEvent& e)
{
	if (e.shield < 0)
	{
		AkUniqueID event_id;
		if (e.shield < -1)
			event_id = AK::EVENTS::PLAY_DRONE_DAMAGE_LARGE;
		else
			event_id = AK::EVENTS::PLAY_DRONE_DAMAGE_SMALL;

		Entity* src = e.source.ref();
		if (src && src != entity() && has<PlayerControlHuman>() && get<PlayerControlHuman>()->local())
		{
			// spatialized damage sounds
			Vec3 offset = Vec3::normalize(e.source.ref()->get<Transform>()->absolute_pos() - get<Transform>()->absolute_pos());
			get<Audio>()->post_offset(event_id, offset * DRONE_SHIELD_RADIUS * 2.0f);
		}
		else
		{
			if (has<Audio>())
				get<Audio>()->post_unattached(event_id);
			else
				Audio::post_global(event_id, get<Transform>()->absolute_pos());
		}
	}
	else if (e.shield > 0)
	{
		if (get<Health>()->shield == 1)
		{
			if (has<Audio>())
				get<Audio>()->post(AK::EVENTS::PLAY_SHIELD_RESTORE_INNER);
			else
				Audio::post_global(AK::EVENTS::PLAY_SHIELD_RESTORE_INNER, Vec3::zero, get<Transform>());
		}
		else if (get<Health>()->shield == 2)
		{
			if (has<Audio>())
				get<Audio>()->post(AK::EVENTS::PLAY_SHIELD_RESTORE_OUTER);
			else
				Audio::post_global(AK::EVENTS::PLAY_SHIELD_RESTORE_OUTER, Vec3::zero, get<Transform>());
		}
	}
}

void alpha_scale_lerp(const Update& u, r32* alpha, r32 alpha_target, r32* scale, r32 scale_target)
{
	const r32 anim_time = 0.3f;
	{
		r32 alpha_speed = (DRONE_SHIELD_ALPHA / anim_time) * u.time.delta;
		if (*alpha > alpha_target)
			*alpha = vi_max(alpha_target, *alpha - alpha_speed);
		else
			*alpha = vi_min(alpha_target, *alpha + alpha_speed);
	}

	if (scale)
	{
		r32 scale_speed = (DRONE_OVERSHIELD_RADIUS * DRONE_SHIELD_VIEW_RATIO / anim_time) * u.time.delta;
		if (*scale > scale_target)
			*scale = vi_max(scale_target, *scale - scale_speed);
		else
			*scale = vi_min(scale_target, *scale + scale_speed);
	}
}

void alpha_mask(View* v, RenderMask mask)
{
	if (v->color.w == 0.0f)
		v->mask = 0;
	else
		v->mask = mask;
}

void Shield::update_client(const Update& u)
{
	if (!inner.ref() || !outer.ref() || !active_armor.ref())
		return;

	Vec3 offset_pos = has<SkinnedModel>() ? get<SkinnedModel>()->offset.translation() : get<View>()->offset.translation();
	RenderMask mask = has<SkinnedModel>() ? get<SkinnedModel>()->mask : get<View>()->mask;

	{
		// inner shield
		View* inner_view = inner.ref();

		r32 target_alpha;
		r32 target_scale;
		if (get<Health>()->shield > 0)
		{
			target_alpha = DRONE_SHIELD_ALPHA;
			target_scale = DRONE_SHIELD_RADIUS * DRONE_SHIELD_VIEW_RATIO;
		}
		else
		{
			target_alpha = 0.0f;
			target_scale = 8.0f;
		}
		r32 inner_scale = inner_view->offset.m[0][0];
		alpha_scale_lerp(u, &inner_view->color.w, target_alpha, &inner_scale, target_scale);
		alpha_mask(inner_view, mask);
		if (inner_view->color.w == 0.0f)
			inner_scale = 0.0f;
		inner_view->offset.make_transform(offset_pos, Vec3(inner_scale), Quat::identity);
	}

	{
		// outer shield
		View* outer_view = outer.ref();

		r32 target_alpha;
		r32 target_scale;
		if (get<Health>()->shield > 1)
		{
			target_alpha = DRONE_OVERSHIELD_ALPHA;
			target_scale = DRONE_OVERSHIELD_RADIUS * DRONE_SHIELD_VIEW_RATIO;
		}
		else
		{
			target_alpha = 0.0f;
			target_scale = 10.0f;
		}
		r32 outer_scale = outer_view->offset.m[0][0];
		alpha_scale_lerp(u, &outer_view->color.w, target_alpha, &outer_scale, target_scale);
		alpha_mask(outer_view, mask);
		if (outer_view->color.w == 0.0f)
			outer_scale = 0.0f;
		outer_view->offset.make_transform(offset_pos, Vec3(outer_scale), Quat::identity);
	}

	{
		// active armor
		View* armor_view = active_armor.ref();
		if (get<Health>()->active_armor())
			armor_view->color.w = 1.0f;
		else
			armor_view->color.w = vi_max(0.0f, armor_view->color.w + u.time.delta * -5.0f);
		alpha_mask(armor_view, mask);
		armor_view->offset.make_transform(offset_pos, Vec3(DRONE_OVERSHIELD_RADIUS * DRONE_SHIELD_VIEW_RATIO * 1.1f), Quat::identity);
	}
}

BatteryEntity::BatteryEntity(const Vec3& p, SpawnPoint* spawn, AI::Team team)
{
	create<Transform>()->pos = p;

	View* model = create<View>();
	model->color = Team::color_neutral();
	model->mesh = Asset::Mesh::battery;
	model->shader = Asset::Shader::standard;
	model->team = s8(team);

	create<Rectifier>(team);

	create<Target>();

	create<Health>(BATTERY_HEALTH, BATTERY_HEALTH);

	Battery* battery = create<Battery>();
	battery->team = team;
	battery->energy = BATTERY_ENERGY;
	battery->spawn_point = spawn;

	model->offset.scale(Vec3(BATTERY_RADIUS - 0.2f));

	RigidBody* body = create<RigidBody>(RigidBody::Type::Sphere, Vec3(BATTERY_RADIUS), 0.1f, CollisionTarget, ~CollisionAllTeamsForceField & ~CollisionWalker & ~CollisionMinionMoving);
	body->set_damping(0.5f, 0.5f);
	body->set_ccd(true);

	create<PointLight>()->radius = 8.0f;

	if (spawn)
	{
		Entity* e = World::create<Empty>();
		Transform* transform = e->get<Transform>();
		transform->parent = spawn->get<Transform>();
		transform->pos.z = FORCE_FIELD_BASE_OFFSET;
		PointLight* light = e->add<PointLight>();
		light->type = PointLight::Type::Shockwave;
		light->radius = FORCE_FIELD_RADIUS;
		Net::finalize_child(e);
		battery->force_field_visualization = e;
	}
}

void Battery::killed(Entity* e)
{
	if (Game::level.local)
		set_team(AI::TeamNone, e);
}

Battery* Battery::closest(AI::TeamMask mask, const Vec3& pos, r32* distance)
{
	Battery* closest = nullptr;
	r32 closest_distance = FLT_MAX;

	for (auto i = list.iterator(); !i.is_last(); i.next())
	{
		if (AI::match(i.item()->team, mask))
		{
			r32 d = (i.item()->get<Transform>()->absolute_pos() - pos).length_squared();
			if (d < closest_distance)
			{
				closest = i.item();
				closest_distance = d;
			}
		}
	}

	if (distance)
	{
		if (closest)
			*distance = sqrtf(closest_distance);
		else
			*distance = FLT_MAX;
	}

	return closest;
}

s32 Battery::count(AI::TeamMask m)
{
	s32 count = 0;
	for (auto i = list.iterator(); !i.is_last(); i.next())
	{
		if (AI::match(i.item()->team, m))
			count++;
	}
	return count;
}

r32 Battery::Comparator::priority(const Ref<Battery>& p)
{
	return (p.ref()->get<Transform>()->absolute_pos() - me).length_squared() * (closest_first ? 1.0f : -1.0f);
}

s32 Battery::Comparator::compare(const Ref<Battery>& a, const Ref<Battery>& b)
{
	r32 pa = priority(a);
	r32 pb = priority(b);
	if (pa > pb)
		return 1;
	else if (pa == pb)
		return 0;
	else
		return -1;
}

void Battery::sort_all(const Vec3& pos, Array<Ref<Battery>>* result, b8 closest_first, AI::TeamMask mask)
{
	Comparator key;
	key.me = pos;
	key.closest_first = closest_first;
	result->length = 0;

	for (auto i = list.iterator(); !i.is_last(); i.next())
	{
		if (AI::match(i.item()->team, mask))
			result->add(i.item());
	}

	Quicksort::sort<Ref<Battery>, Comparator>(result->data, 0, result->length, &key);
}

void battery_spawn_force_field(Battery* b)
{
	Vec3 pos;
	Quat rot;
	b->spawn_point.ref()->get<Transform>()->absolute(&pos, &rot);
	ParticleEffect::spawn(ParticleEffect::Type::SpawnBatteryForceField, pos + rot * Vec3(0, 0, FORCE_FIELD_BASE_OFFSET), rot, nullptr, nullptr, b->team);
}

void Battery::awake()
{
	if (Game::level.mode == Game::Mode::Pvp)
		PlayerHuman::log_add(_(strings::battery_added));
	link_arg<const TargetEvent&, &Battery::hit>(get<Target>()->target_hit);
	link_arg<Entity*, &Battery::killed>(get<Health>()->killed);
	set_team_client(team);
	if (Game::level.local && team != AI::TeamNone)
		battery_spawn_force_field(this);
}

Battery::~Battery()
{
	if (Game::level.local && force_field_visualization.ref())
		World::remove_deferred(force_field_visualization.ref());
}

void Battery::hit(const TargetEvent& e)
{
	if (e.hit_by->has<Drone>() && (e.hit_by->get<Drone>()->current_ability != Ability::None && AbilityInfo::list[s32(e.hit_by->get<Drone>()->current_ability)].type == AbilityInfo::Type::Shoot))
		set_team(AI::TeamNone, e.hit_by);
	else
		set_team(e.hit_by->get<AIAgent>()->team, e.hit_by);
}

void Battery::set_team_client(AI::Team t)
{
	if (team != AI::TeamNone && team != t)
		PlayerHuman::log_add(_(strings::battery_lost), AI::TeamNone, 1 << team);

	team = t;
	get<View>()->team = s8(t);
	if (has<Rectifier>())
		get<Rectifier>()->team = t;
	spawn_point.ref()->set_team(t);
}

namespace BatteryNet
{
	enum class Message : s8
	{
		Energy,
		Team,
		count,
	};

	b8 set_team(Battery* b, AI::Team t, Entity* caused_by)
	{
		vi_assert(Game::level.local);

		// must be neutral or owned by an enemy
		b->get<Health>()->reset_hp();

		using Stream = Net::StreamWrite;
		Net::StreamWrite* p = Net::msg_new(Net::MessageType::Battery);
		{
			Message msg = Message::Team;
			serialize_enum(p, Message, msg);
		}
		{
			Ref<Battery> ref = b;
			serialize_ref(p, ref);
		}
		serialize_s8(p, t);
		{
			Ref<Entity> caused_by_ref = caused_by;
			serialize_ref(p, caused_by_ref);
		}
		Net::msg_finalize(p);
		return true;
	}

	b8 set_energy(Battery* b, s16 energy)
	{
		vi_assert(Game::level.local);

		using Stream = Net::StreamWrite;
		Net::StreamWrite* p = Net::msg_new(Net::MessageType::Battery);
		{
			Message msg = Message::Energy;
			serialize_enum(p, Message, msg);
		}
		{
			Ref<Battery> ref = b;
			serialize_ref(p, ref);
		}
		serialize_s16(p, energy);
		Net::msg_finalize(p);
		return true;
	}
}

b8 Battery::net_msg(Net::StreamRead* p)
{
	using Stream = Net::StreamRead;
	BatteryNet::Message msg;
	serialize_enum(p, BatteryNet::Message, msg);
	switch (msg)
	{
		case BatteryNet::Message::Energy:
		{
			Ref<Battery> ref;
			serialize_ref(p, ref);
			s16 energy;
			serialize_s16(p, energy);

			ref.ref()->energy = energy;

			if (energy == 0) // this thing is exhausted
			{
				if (SpawnPoint* spawn = ref.ref()->spawn_point.ref())
					spawn->set_team(AI::TeamNone);
			}

			break;
		}
		case BatteryNet::Message::Team:
		{
			Ref<Battery> ref;
			serialize_ref(p, ref);
			AI::Team t;
			serialize_s8(p, t);
			Ref<Entity> caused_by;
			serialize_ref(p, caused_by);

			Battery* pickup = ref.ref();
			if (caused_by.ref() && caused_by.ref()->has<AIAgent>() && t == caused_by.ref()->get<AIAgent>()->team)
				Audio::post_global(AK::EVENTS::PLAY_BATTERY_CAPTURE, Vec3::zero, pickup->get<Transform>());
			pickup->set_team_client(t);
			break;
		}
		default:
			vi_assert(false);
			break;
	}

	return true;
}

// returns true if we were successfully captured
// the second parameter is the entity that caused the ownership change
b8 Battery::set_team(AI::Team t, Entity* caused_by)
{
	if (t == team)
		return false;
	else
	{
		get<Health>()->add(BATTERY_HEALTH);
		BatteryNet::set_team(this, t, caused_by);
		if (t != AI::TeamNone)
			battery_spawn_force_field(this);
		return true;
	}
}

r32 Battery::particle_accumulator;
r32 Battery::increment_timer;
void Battery::update_all(const Update& u)
{
	// normal particles
	const r32 interval = 0.1f;
	particle_accumulator += u.time.delta;
	while (particle_accumulator > interval)
	{
		particle_accumulator -= interval;
		for (auto i = list.iterator(); !i.is_last(); i.next())
		{
			Vec3 pos = i.item()->get<Transform>()->absolute_pos();

			Particles::tracers.add
			(
				pos + Quat::euler(0.0f, mersenne::randf_co() * PI * 2.0f, (mersenne::randf_co() - 0.5f) * PI) * Vec3(0, 0, mersenne::randf_co() * 0.6f),
				Vec3::zero,
				0
			);
		}
	}

	b8 increment = false;
	increment_timer -= u.time.delta;
	if (increment_timer < 0.0f)
	{
		increment = Game::level.has_feature(Game::FeatureLevel::Batteries);
		increment_timer += Game::level.has_feature(Game::FeatureLevel::All) ? BATTERY_ENERGY_INCREMENT_TIME : 1.0f;
	}

	for (auto i = list.iterator(); !i.is_last(); i.next())
	{
		if (Game::level.local && increment)
		{
			i.item()->get<Health>()->add(BATTERY_HEALTH / 3);

			if (i.item()->team != AI::TeamNone)
			{
				s32 increment = BATTERY_ENERGY_INCREMENT / Game::level.battery_spawn_group_size;

				if (Game::session.config.game_type == GameType::Assault && i.item()->team == 0) // assault defenders do not drain battery
					increment /= 2;
				else
				{
					// drain battery

					BatteryNet::set_energy(i.item(), vi_max(0, i.item()->energy - BATTERY_ENERGY_INCREMENT));
					if (i.item()->energy == 0)
					{
						// battery exhausted
						{
							Vec3 pos;
							Quat rot;
							i.item()->get<Transform>()->absolute(&pos, &rot);
							ParticleEffect::spawn(ParticleEffect::Type::Explosion, pos, rot);
						}
						World::remove_deferred(i.item()->entity());
						if (Game::session.config.game_type == GameType::Assault)
						{
							Team::match_time = vi_max(0.0f, vi_min(Game::session.config.time_limit(), Team::match_time) - (BATTERY_TIME_REWARD / r32(Game::level.battery_spawn_group_size)));
#if SERVER
							Net::Server::sync_time();
#endif
						}
					}
				}

				for (auto j = PlayerManager::list.iterator(); !j.is_last(); j.next())
				{
					if (j.item()->team.ref()->team() == i.item()->team)
						j.item()->add_energy(increment);
				}
			}
		}
	}
}

void Battery::awake_all()
{
	increment_timer = 0.0f;
}

SpawnPointEntity::SpawnPointEntity(AI::Team team, b8 visible)
{
	create<Transform>();

	SpawnPoint* sp = create<SpawnPoint>();
	sp->team = team;

	if (visible)
	{
		View* view = create<View>();
		view->mesh = Asset::Mesh::spawn_main;
		view->shader = Asset::Shader::culled;
		view->team = s8(team);
		view->color = Team::color_neutral();

		PointLight* light = create<PointLight>();
		light->color = Team::color_neutral().xyz();
		light->offset.z = 2.0f;
		light->radius = 12.0f;
		light->team = s8(team);

		Entity* upgrade_station = World::create<UpgradeStationEntity>(sp);
		upgrade_station->get<Transform>()->parent = get<Transform>();
		Net::finalize_child(upgrade_station);

		create<RigidBody>(RigidBody::Type::Mesh, Vec3(1.0f), 0.0f, CollisionStatic | CollisionParkour, ~CollisionStatic & ~CollisionAudio & ~CollisionParkour & ~CollisionInaccessible & ~CollisionElectric & ~CollisionGlass, Asset::Mesh::spawn_collision);
	}
}

void SpawnPoint::set_team(AI::Team t)
{
	team = t;
	get<View>()->team = s8(t);
	get<PointLight>()->team = s8(t);
}

Battery* SpawnPoint::battery() const
{
	for (auto i = Battery::list.iterator(); !i.is_last(); i.next())
	{
		if (i.item()->spawn_point.ref() == this)
			return i.item();
	}
	return nullptr;
}

b8 drone_can_spawn(const Vec3& pos)
{
	for (auto i = Drone::list.iterator(); !i.is_last(); i.next())
	{
		if (LMath::ray_sphere_intersect(pos + Vec3(0, 3, 0), pos + Vec3(0, -3, 0), i.item()->get<Transform>()->absolute_pos(), DRONE_SHIELD_RADIUS))
			return false;
	}
	return true;
}

SpawnPosition SpawnPoint::spawn_position() const
{
	SpawnPosition result;
	Quat rot;
	get<Transform>()->absolute(&result.pos, &rot);
	Vec3 dir = rot * Vec3(0, 1, 0);
	result.angle = atan2f(dir.x, dir.z);
	result.pos += rot * Vec3(0, 0, DRONE_RADIUS);

	s32 j = 0;
	Vec3 p = result.pos;
	while (!drone_can_spawn(p) && j < (MAX_PLAYERS / 2))
	{
		p = result.pos + (rot * Quat::euler(r32(j) * PI * (2.0f / (MAX_PLAYERS / 2)), 0, 0)) * Vec3(2.0f, 0, 0);
		j++;
	}
	result.pos = p;
	return result;
}

PlayerManager* minion_spawn_get_owner_spawner(MinionSpawner* m)
{
	return m->owner.ref();
}

PlayerManager* minion_spawn_get_owner_point(SpawnPoint*)
{
	return nullptr;
}

template<typename T> void minion_spawn_all(const Update& u, PlayerManager* (*owner_get)(T*), r32 interval_multiplier)
{
	const s32 minion_group = 3;
	const r32 minion_initial_delay = 60.0f * interval_multiplier;
	const r32 minion_spawn_interval = 13.0f * interval_multiplier; // time between individual minions spawning
	const r32 minion_group_interval = minion_spawn_interval * 13.0f; // time between minion groups spawning; must be a multiple of minion_spawn_interval

	for (auto i = T::list.iterator(); !i.is_last() && Minion::list.count() < MAX_MINIONS; i.next())
	{
		if (i.item()->team != AI::TeamNone)
		{
			r32 t = (Game::time.total - minion_initial_delay) + i.index * minion_spawn_interval * -0.5f;
			if (t > 0.0f)
			{
				s32 index = s32(t / minion_spawn_interval);
				s32 index_last = s32((t - u.time.delta) / minion_spawn_interval);
				if (index != index_last && (index % s32(minion_group_interval / minion_spawn_interval)) <= minion_group)
				{
					Vec3 pos;
					Quat rot;
					i.item()->template get<Transform>()->absolute(&pos, &rot);
					pos += rot * Vec3(0, 0, 2.0f);
					pos.y -= 2.0f;
					ParticleEffect::spawn(ParticleEffect::Type::SpawnMinion, pos, Quat::identity, nullptr, owner_get(i.item()), i.item()->team);
				}
			}
		}
	}
}

// returns true if entity is still alive afterward
b8 entity_minion_attach_update(Transform* transform, Vec3* abs_pos_attached, AssetID mesh_normal, r32 offset_minion, r32 offset_normal, r32 scale = 1.0f)
{
	View* view = transform->get<View>();
	if (Transform* parent = transform->parent.ref())
	{
		if (parent->has<Minion>())
		{
			Vec3 pos(0.1f, 0.0f, -0.12f - offset_minion);
			Quat rot = Quat::euler(0, PI, 0);
			parent->get<Animator>()->to_local(Asset::Bone::character_spine, &pos, &rot);
			view->offset.make_transform(pos, Vec3(scale), rot);
			transform->get<Target>()->local_offset = pos;
			*abs_pos_attached = parent->to_world(pos);
		}
		else
		{
			transform->get<Target>()->local_offset = Vec3::zero;
			view->mesh = mesh_normal;
			view->offset = Mat4::make_scale(Vec3(scale));
		}
		return true;
	}
	else
	{
		view->mesh = mesh_normal;
		view->offset = Mat4::make_scale(Vec3(scale));
		transform->get<Target>()->local_offset = Vec3::zero;

		if (Game::level.local)
		{
			// parent is gone; reattach to something
			btCollisionWorld::ClosestRayResultCallback ray_callback(*abs_pos_attached, *abs_pos_attached + Vec3(0, -4.0f, 0));
			Physics::raycast(&ray_callback, CollisionStatic);
			if (ray_callback.hasHit() && !(ray_callback.m_collisionObject->getBroadphaseHandle()->m_collisionFilterGroup & DRONE_INACCESSIBLE_MASK))
			{
				Entity* entity = &Entity::list[ray_callback.m_collisionObject->getUserIndex()];
				transform->pos = ray_callback.m_hitPointWorld + (ray_callback.m_hitNormalWorld * offset_normal);
				transform->rot = Quat::look(ray_callback.m_hitNormalWorld);
				transform->reparent(entity->get<Transform>());
				return true;
			}
			else
			{
				transform->get<Health>()->kill(nullptr);
				return false;
			}
		}
		else
			return true;
	}
}

void MinionSpawner::update_all(const Update& u)
{
	if (Game::level.local
		&& Game::level.mode == Game::Mode::Pvp
		&& Game::level.has_feature(Game::FeatureLevel::TutorialAll)
		&& Team::match_state == Team::MatchState::Active)
	{
		minion_spawn_all<MinionSpawner>(u, minion_spawn_get_owner_spawner, 0.75f);
		minion_spawn_all<SpawnPoint>(u, minion_spawn_get_owner_point, 1.0f);
	}

	for (auto i = list.iterator(); !i.is_last(); i.next())
	{
		if (!entity_minion_attach_update(i.item()->get<Transform>(), &i.item()->abs_pos_attached, Asset::Mesh::minion_spawner_main, MINION_SPAWNER_RADIUS, MINION_SPAWNER_RADIUS))
			continue;
	}
}


namespace UpgradeStationNet
{
	b8 set_drone(UpgradeStation* u, Drone* d)
	{
		using Stream = Net::StreamWrite;
		Net::StreamWrite* p = Net::msg_new(Net::MessageType::UpgradeStation);

		{
			Ref<UpgradeStation> ref = u;
			serialize_ref(p, ref);
		}

		{
			Ref<Drone> ref = d;
			serialize_ref(p, ref);
		}

		Net::msg_finalize(p);
		return true;
	}
}

#define UPGRADE_STATION_ANIM_TIME 0.5f
b8 UpgradeStation::net_msg(Net::StreamRead* p, Net::MessageSource src)
{
	using Stream = Net::StreamRead;
	Ref<UpgradeStation> ref;
	serialize_ref(p, ref);
	Ref<Drone> drone;
	serialize_ref(p, drone);

	UpgradeStation* u = ref.ref();
	if (u)
	{
		Drone* d = drone.ref();
		if ((!u->drone.ref() && d
			&& d->state() == Drone::State::Crawl)
			|| (u->drone.ref() && !d))
		{
#if SERVER
			if (src == Net::MessageSource::Remote)
			{
				// repeat this message to all clients, including ourselves
				// we will process the message in the `else` statement below
				UpgradeStationNet::set_drone(u, d);
			}
			else
#endif
			{
				if (d)
				{
					if (ref.ref()->drone.ref() != d)
					{
						// drone must drop flag if it's holding one
						if (Game::level.local)
						{
							Flag* flag = d->flag.ref();
							if (flag)
								flag->drop();
						}
						d->flag = nullptr;

						ref.ref()->drone = d;
						ref.ref()->timer = UPGRADE_STATION_ANIM_TIME - ref.ref()->timer;
						ref.ref()->mode = Mode::Activating;
						Audio::post_global(AK::EVENTS::PLAY_UPGRADE_STATION_ENTER, Vec3::zero, ref.ref()->get<Transform>());
					}
				}
				else if (ref.ref()->mode == Mode::Activating)
				{
					ref.ref()->timer = UPGRADE_STATION_ANIM_TIME - ref.ref()->timer;
					ref.ref()->mode = Mode::Deactivating;
					Audio::post_global(AK::EVENTS::PLAY_UPGRADE_STATION_EXIT, Vec3::zero, ref.ref()->get<Transform>());
				}
			}
		}
	}

	return true;
}

// returns the upgrade station the given drone is in range of, if any
UpgradeStation* UpgradeStation::drone_at(const Drone* drone)
{
	if (!drone)
		return nullptr;

	AI::Team team = drone->get<AIAgent>()->team;
	for (auto i = list.iterator(); !i.is_last(); i.next())
	{
		AI::Team spawn_team = i.item()->spawn_point.ref()->team;
		if ((spawn_team == AI::TeamNone || spawn_team == team)
			&& i.item()->get<PlayerTrigger>()->is_triggered(drone->entity())
			&& !i.item()->drone.ref())
			return i.item();
	}

	return nullptr;
}

// returns the upgrade station the given drone is currently inside, if any
UpgradeStation* UpgradeStation::drone_inside(const Drone* drone)
{
	for (auto i = list.iterator(); !i.is_last(); i.next())
	{
		if (i.item()->drone.ref() == drone)
			return i.item();
	}
	return nullptr;
}

UpgradeStation* UpgradeStation::closest_available(AI::Team team, const Vec3& pos, r32* distance)
{
	UpgradeStation* closest = nullptr;
	r32 closest_distance = FLT_MAX;

	for (auto i = list.iterator(); !i.is_last(); i.next())
	{
		AI::Team station_team = i.item()->spawn_point.ref()->team;
		if ((station_team == AI::TeamNone || station_team == team) && !i.item()->drone.ref())
		{
			r32 d = (i.item()->get<Transform>()->absolute_pos() - pos).length_squared();
			if (d < closest_distance)
			{
				closest = i.item();
				closest_distance = d;
			}
		}
	}

	if (distance)
	{
		if (closest)
			*distance = sqrtf(closest_distance);
		else
			*distance = FLT_MAX;
	}

	return closest;
}

#define UPGRADE_STATION_OFFSET Vec3(0, 0, -0.2f)
void UpgradeStation::update(const Update& u)
{
	if (mode == Mode::Activating && !drone.ref())
	{
		// drone disappeared on us; flip back over automatically
		mode = Mode::Deactivating;
		timer = UPGRADE_STATION_ANIM_TIME;
		Audio::post_global(AK::EVENTS::PLAY_UPGRADE_STATION_EXIT, Vec3::zero, get<Transform>());
	}

	if (timer > 0.0f)
	{
		timer = vi_max(0.0f, timer - u.time.delta);
		if (timer == 0.0f && mode == Mode::Deactivating)
			drone = nullptr;
	}

	if (drone.ref() && mode != Mode::Deactivating)
		get<View>()->team = drone.ref()->get<AIAgent>()->team;
	else
		get<View>()->team = AI::TeamNone;

	if (timer == 0.0f)
	{
		get<View>()->mesh = Asset::Mesh::spawn_collision;
		get<View>()->offset = Mat4::identity;
	}
	else
	{
		get<View>()->mesh = Asset::Mesh::spawn_upgrade_station;
		get<View>()->offset.make_transform(timer > 0.0f ? UPGRADE_STATION_OFFSET : Vec3::zero, Vec3(1.0f), rotation());
	}
}

Quat UpgradeStation::rotation() const
{
	r32 blend = timer / UPGRADE_STATION_ANIM_TIME;
	if (mode == Mode::Activating)
		blend = Ease::quad_out<r32>(1.0f - blend);
	else
		blend = Ease::quad_in<r32>(blend);
	return Quat::euler(0, blend * PI, 0);
}

void UpgradeStation::transform(Vec3* pos, Quat* rot) const
{
	get<Transform>()->to_local(pos, rot);

	*pos -= UPGRADE_STATION_OFFSET;

	Quat my_rot = rotation();

	*pos = my_rot * *pos;
	*rot = my_rot * *rot;

	*pos += UPGRADE_STATION_OFFSET;

	get<Transform>()->to_world(pos, rot);
}

void UpgradeStation::drone_enter(Drone* d)
{
	UpgradeStationNet::set_drone(this, d);
}

void UpgradeStation::drone_exit()
{
	if (drone.ref())
		UpgradeStationNet::set_drone(this, nullptr);
}

UpgradeStationEntity::UpgradeStationEntity(SpawnPoint* p)
{
	create<Transform>();

	create<PlayerTrigger>()->radius = UPGRADE_STATION_RADIUS;

	UpgradeStation* u = create<UpgradeStation>();
	u->spawn_point = p;

	View* view = create<View>();
	view->mesh = Asset::Mesh::spawn_collision;
	view->shader = Asset::Shader::culled;
	view->team = AI::TeamNone;
	view->color = Team::color_neutral();
}

RectifierEntity::RectifierEntity(PlayerManager* owner, const Vec3& abs_pos, const Quat& abs_rot, Transform* parent)
{
	Transform* transform = create<Transform>();
	if (parent && parent->has<Minion>())
		transform->parent = parent; // parent at 0, 0, 0
	else
	{
		transform->pos = abs_pos;
		transform->rot = abs_rot;
		transform->reparent(parent);
	}

	View* model = create<View>();
	model->mesh = Asset::Mesh::rectifier_normal;
	if (parent && parent->has<Minion>())
		model->mesh = Asset::Mesh::rectifier_attached;
	model->team = s8(owner->team.ref()->team());
	model->shader = Asset::Shader::culled;
	model->offset.scale(Vec3(RECTIFIER_RADIUS));
	model->color = Team::color_neutral();

	create<Health>(RECTIFIER_HEALTH, RECTIFIER_HEALTH, DRONE_SHIELD_AMOUNT, DRONE_SHIELD_AMOUNT);

	create<Rectifier>(owner->team.ref()->team(), owner);

	create<Target>();

	create<Shield>();
}

Rectifier::Rectifier(AI::Team t, PlayerManager* o)
	: team(t), owner(o)
{
}

void Rectifier::awake()
{
	if (Transform* parent = get<Transform>()->parent.ref())
	{
		if (parent->has<Minion>())
			parent->get<Minion>()->carrying = entity();
	}
	if (!has<Battery>())
		link_arg<Entity*, &Rectifier::killed_by>(get<Health>()->killed);
}

void Rectifier::killed_by(Entity* e)
{
	vi_assert(!has<Battery>());
	PlayerManager::entity_killed_by(entity(), e);
	if (Game::level.local)
	{
		{
			Vec3 pos;
			Quat rot;
			get<Transform>()->absolute(&pos, &rot);
			ParticleEffect::spawn(ParticleEffect::Type::Explosion, pos, rot);
		}
		World::remove_deferred(entity());
	}
}

void rectifier_update(s32 rectifier_index, const Vec3& rectifier_pos, Entity* e, PlayerManager* owner)
{
	Vec3 pos = e->get<Target>()->absolute_pos();
	if ((pos - rectifier_pos).length_squared() < (RECTIFIER_RANGE + 2.0f) * (RECTIFIER_RANGE + 2.0f))
	{
		if (!Rectifier::heal_targets.get(e->id()) || Rectifier::heal_counts[e->id()] < RECTIFIER_HEAL_MAX_CONCURRENT)
		{
			Rectifier::heal_targets.set(e->id(), true);
			Rectifier::heal_counts[e->id()]++;

			r32 time = Game::time.total + (rectifier_index + e->id()) * 0.5f;

			// effects
			{
				r32 heal_effect_lerp = (time - (s32(time / RECTIFIER_HEAL_INTERVAL) * RECTIFIER_HEAL_INTERVAL)) / RECTIFIER_HEAL_INTERVAL;

				const r32 particle_interval = RECTIFIER_HEAL_INTERVAL / 8.0f;
				if (s32(time / particle_interval) != s32((time - Game::time.delta) / particle_interval))
				{
					Particles::fast_tracers.add
					(
						Vec3::lerp(heal_effect_lerp, rectifier_pos, pos),
						Vec3::zero,
						0
					);
				}

				if (heal_effect_lerp > 0.25f)
				{
					heal_effect_lerp = (heal_effect_lerp - 0.25f) / 0.75f;

					InstanceVertex* instance = Rectifier::heal_effects.add();
					r32 scale;
					if (e->has<Minion>())
						scale = Ease::cubic_in<r32>(heal_effect_lerp, 0.5f, 0.05f);
					else
						scale = Ease::cubic_in<r32>(heal_effect_lerp, 1.0f, 0.15f);
					instance->world_matrix.make_transform(pos, Vec3(scale), Quat::identity);
					instance->color = Vec4(1, 1, 1, Ease::cubic_in<r32>(heal_effect_lerp));
				}
			}

			b8 do_heal = Game::level.local && s32(time / RECTIFIER_HEAL_INTERVAL) != s32((time - Game::time.delta) / RECTIFIER_HEAL_INTERVAL);
			if (do_heal) // apply actual healing
			{
				Health* health = e->get<Health>();
				if (health->hp > 0)
					health->add(RECTIFIER_HEAL_AMOUNT);
			}
		}
	}
}

Array<InstanceVertex> Rectifier::heal_effects;
s8 Rectifier::heal_counts[MAX_ENTITIES];
Bitmask<MAX_ENTITIES> Rectifier::heal_targets;
void Rectifier::update_all(const Update& u)
{
	// reset heal counts
	if (heal_targets.any())
	{
		for (s32 i = heal_targets.start; i < heal_targets.end; i = heal_targets.next(i))
			heal_counts[i] = 0;
		heal_targets.clear();
	}

	heal_effects.length = 0;
	r32 last_time = u.time.total - u.time.delta;
	for (auto i = list.iterator(); !i.is_last(); i.next())
	{
		Transform* transform = i.item()->get<Transform>();

		if (!i.item()->has<Battery>())
		{
			if (!entity_minion_attach_update(transform, &i.item()->abs_pos_attached, Asset::Mesh::rectifier_normal, RECTIFIER_RADIUS, RECTIFIER_RADIUS, RECTIFIER_RADIUS))
				continue;
		}

		if (i.item()->team != AI::TeamNone)
		{
			Vec3 me = transform->absolute_pos();

			PlayerManager* owner = i.item()->owner.ref();

			// buff friendly turrets, force fields, and minions
			for (auto j = Turret::list.iterator(); !j.is_last(); j.next())
			{
				if (j.item()->team == i.item()->team)
					rectifier_update(i.index, me, j.item()->entity(), owner);
			}

			for (auto j = ForceField::list.iterator(); !j.is_last(); j.next())
			{
				if (!(j.item()->flags & (ForceField::FlagPermanent | ForceField::FlagInvincible)) && j.item()->team == i.item()->team)
					rectifier_update(i.index, me, j.item()->entity(), owner);
			}

			for (auto j = Minion::list.iterator(); !j.is_last(); j.next())
			{
				if (j.item()->get<AIAgent>()->team == i.item()->team)
					rectifier_update(i.index, me, j.item()->entity(), owner);
			}

			for (auto j = MinionSpawner::list.iterator(); !j.is_last(); j.next())
			{
				if (j.item()->team == i.item()->team)
					rectifier_update(i.index, me, j.item()->entity(), owner);
			}
		}
	}
}

void Rectifier::draw_alpha_all(const RenderParams& params)
{
	if (!params.camera->mask || heal_effects.length == 0)
		return;

	Loader::mesh_instanced(Asset::Mesh::heal_effect);

	const r32 f_radius = 1.0f;

	RenderSync* sync = params.sync;

	if (params.update_instances())
	{
		sync->write(RenderOp::UpdateInstances);
		sync->write(Asset::Mesh::heal_effect);
		sync->write(heal_effects.length);
		sync->write<InstanceVertex>(heal_effects.data, heal_effects.length);
	}

	Loader::shader_permanent(Asset::Shader::flat_instanced);

	sync->write(RenderOp::Shader);
	sync->write(Asset::Shader::flat_instanced);
	sync->write(params.technique);

	Mat4 vp = params.view_projection;

	sync->write(RenderOp::Uniform);
	sync->write(Asset::Uniform::vp);
	sync->write(RenderDataType::Mat4);
	sync->write<s32>(1);
	sync->write<Mat4>(vp);

	sync->write(RenderOp::Uniform);
	sync->write(Asset::Uniform::v);
	sync->write(RenderDataType::Mat4);
	sync->write<s32>(1);
	sync->write<Mat4>(params.view);

	sync->write(RenderOp::Uniform);
	sync->write(Asset::Uniform::diffuse_color);
	sync->write(RenderDataType::Vec4);
	sync->write<s32>(1);
	sync->write<Vec4>(Vec4(1, 1, 1, 0.5f));

	sync->write(RenderOp::Instances);
	sync->write(Asset::Mesh::heal_effect);
}

// returns true if given team can see the given position
b8 Rectifier::can_see(AI::TeamMask team_mask, const Vec3& pos, const Vec3& normal)
{
	for (auto i = list.iterator(); !i.is_last(); i.next())
	{
		if (AI::match(i.item()->team, team_mask))
		{
			Vec3 to_rectifier = i.item()->get<Transform>()->absolute_pos() - pos;
			if (to_rectifier.length_squared() < RECTIFIER_RANGE * RECTIFIER_RANGE && to_rectifier.dot(normal) > 0.0f)
				return true;
		}
	}
	return false;
}

Rectifier* Rectifier::closest(AI::TeamMask mask, const Vec3& pos, r32* distance)
{
	Rectifier* closest = nullptr;
	r32 closest_distance = FLT_MAX;

	for (auto i = list.iterator(); !i.is_last(); i.next())
	{
		if (AI::match(i.item()->team, mask))
		{
			r32 d = (i.item()->get<Transform>()->absolute_pos() - pos).length_squared();
			if (d < closest_distance)
			{
				closest = i.item();
				closest_distance = d;
			}
		}
	}

	if (distance)
	{
		if (closest)
			*distance = sqrtf(closest_distance);
		else
			*distance = FLT_MAX;
	}

	return closest;
}

namespace FlagNet
{
	b8 state_change(Flag* flag, Flag::StateChange change, Entity* param = nullptr)
	{
		using Stream = Net::StreamWrite;
		Net::StreamWrite* p = Net::msg_new(Net::MessageType::Flag);
		{
			Ref<Flag> ref_flag = flag;
			serialize_ref(p, ref_flag);
		}
		serialize_enum(p, Flag::StateChange, change);

		{
			Ref<Entity> ref = param;
			serialize_ref(p, ref);
		}

		if (change == Flag::StateChange::Dropped)
			serialize_r32_range(p, flag->timer, 0.0f, FLAG_RESTORE_TIME, 16);

		Net::msg_finalize(p);
		return true;
	}
}


b8 Flag::net_msg(Net::StreamRead* p, Net::MessageSource src)
{
	using Stream = Net::StreamRead;
	Ref<Flag> ref;
	serialize_ref(p, ref);
	StateChange change;
	serialize_enum(p, StateChange, change);
	Ref<Entity> param;
	serialize_ref(p, param);
	r32 timer = 0.0f;
	if (change == StateChange::Dropped)
		serialize_r32_range(p, timer, 0.0f, FLAG_RESTORE_TIME, 16);
	if (ref.ref() && (Game::level.local == (src == Net::MessageSource::Loopback)))
	{
		Flag* flag = ref.ref();
		switch (change)
		{
			case StateChange::PickedUp:
			{
				if (param.ref())
				{
					param.ref()->get<Drone>()->flag = flag;
					if (param.ref()->has<PlayerControlHuman>())
					{
						if (param.ref()->get<PlayerControlHuman>()->local())
							param.ref()->get<PlayerControlHuman>()->ability_select(Ability::None);
					}
					else // bot
						param.ref()->get<Drone>()->ability(Ability::None);
				}
				flag->at_base = false;
				flag->get<View>()->mask = 0;

				Audio::post_global(AK::EVENTS::PLAY_NOTIFICATION_LOST);

				{
					char buffer[UI_TEXT_MAX];
					snprintf(buffer, UI_TEXT_MAX, _(strings::flag_captured), _(Team::name_long(flag->team)));
					PlayerHuman::log_add(buffer, flag->team);
				}

				break;
			}
			case StateChange::Dropped:
			{
				flag->at_base = false;
				flag->get<View>()->mask = RENDER_MASK_DEFAULT;
				flag->timer = timer;

				Audio::post_global(AK::EVENTS::PLAY_NOTIFICATION_UNDER_ATTACK);

				{
					char buffer[UI_TEXT_MAX];
					snprintf(buffer, UI_TEXT_MAX, _(strings::flag_dropped), _(Team::name_long(flag->team)));
					PlayerHuman::log_add(buffer, flag->team);
				}

				break;
			}
			case StateChange::Scored:
			{
				// param = PlayerManager who scored
				flag->at_base = true;
				flag->get<View>()->mask = RENDER_MASK_DEFAULT;
				flag->timer = FLAG_RESTORE_TIME;

				Audio::post_global(AK::EVENTS::PLAY_NOTIFICATION_LOST);

				PlayerManager* player = param.ref()->get<PlayerManager>();
				if (player->instance.ref())
					player->instance.ref()->get<Drone>()->flag = nullptr;

				{
					char buffer[UI_TEXT_MAX];
					snprintf(buffer, UI_TEXT_MAX, _(strings::flag_scored), player->username);
					PlayerHuman::log_add(buffer, player->team.ref()->team(), AI::TeamAll, player->flag(PlayerManager::FlagIsVip));
				}
				break;
			}
			case StateChange::Restored:
			{
				flag->at_base = true;
				flag->get<View>()->mask = RENDER_MASK_DEFAULT;
				flag->timer = FLAG_RESTORE_TIME;

				Audio::post_global(AK::EVENTS::PLAY_NOTIFICATION_UNDER_ATTACK);

				{
					char buffer[UI_TEXT_MAX];
					snprintf(buffer, UI_TEXT_MAX, _(strings::flag_restored), _(Team::name_long(flag->team)));
					PlayerHuman::log_add(buffer, flag->team);
				}
				break;
			}
			default:
				vi_assert(false);
				break;
		}
	}
	return true;
}

void Flag::awake()
{
	link_arg<Entity*, &Flag::player_entered_trigger>(get<PlayerTrigger>()->entered);
}

void Flag::player_entered_trigger(Entity* player)
{
	if (Game::level.local
		&& player->get<AIAgent>()->team != team
		&& !get<Transform>()->parent.ref())
	{
		// attach ourselves to player
		get<Transform>()->pos = Vec3::zero;
		get<Transform>()->parent = player->get<Transform>();
		FlagNet::state_change(this, StateChange::PickedUp, player);
	}
}

void Flag::drop()
{
	vi_assert(Game::level.local);
	get<Transform>()->parent = nullptr;
	get<Transform>()->pos = pos_cached;
	FlagNet::state_change(this, StateChange::Dropped);
}

Flag* Flag::for_team(AI::Team t)
{
	for (auto i = list.iterator(); !i.is_last(); i.next())
	{
		if (i.item()->team == t)
			return i.item();
	}
	return nullptr;
}

void flag_build_force_field(Transform* flag, AI::Team team)
{
	Vec3 abs_pos;
	Quat abs_rot;
	flag->absolute(&abs_pos, &abs_rot);

	ParticleEffect::spawn(ParticleEffect::Type::SpawnForceField, abs_pos + abs_rot * Vec3(0, 0, FORCE_FIELD_BASE_OFFSET), abs_rot, flag, nullptr, team);
}

Vec3 flag_base_pos(Transform* flag_base)
{
	Vec3 pos;
	Quat rot;
	flag_base->absolute(&pos, &rot);
	pos += rot * Vec3(0, 0, FLAG_RADIUS);
	return pos;
}

void Flag::update_server(const Update& u)
{
	pos_cached = get<Transform>()->absolute_pos();

	if (get<Transform>()->parent.ref())
	{
		// we're being carried
		// restore timer counts up faster than it counts down
		timer = vi_min(FLAG_RESTORE_TIME, timer + u.time.delta * 1.5f);
		for (auto i = Team::list.iterator(); !i.is_last(); i.next())
		{
			if (i.item()->team() != team
				&& for_team(i.item()->team())->at_base
				&& (i.item()->flag_base.ref()->absolute_pos() - pos_cached).length_squared() < get<PlayerTrigger>()->radius * get<PlayerTrigger>()->radius)
			{
				// score
				PlayerManager* player = get<Transform>()->parent.ref()->get<PlayerCommon>()->manager.ref();
				player->captured_flag();

				FlagNet::state_change(this, StateChange::Scored, player->entity());
				get<Transform>()->parent = nullptr;
				get<Transform>()->pos = pos_cached = flag_base_pos(Team::list[team].flag_base.ref());

				// rebuild enemy force field
				flag_build_force_field(Team::list[team].flag_base.ref(), team);
				break;
			}
		}
	}
	else
	{
		// we're sitting somewhere
		if (!at_base)
		{
			timer = vi_max(0.0f, timer - u.time.delta);
			if (timer == 0.0f)
			{
				FlagNet::state_change(this, StateChange::Restored);
				get<Transform>()->pos = pos_cached = flag_base_pos(Team::list[team].flag_base.ref());
				b8 all_at_base = true;
				for (auto i = list.iterator(); !i.is_last(); i.next())
				{
					if (!i.item()->at_base)
					{
						all_at_base = false;
						break;
					}
				}

				if (all_at_base)
				{
					// all flags are at their bases; rebuild all force fields
					for (auto i = Team::list.iterator(); !i.is_last(); i.next())
						flag_build_force_field(i.item()->flag_base.ref(), i.item()->team());
				}
			}
		}
	}
}

void Flag::update_client(const Update& u)
{
	if (!get<Transform>()->parent.ref() && !at_base)
		timer = vi_max(0.0f, timer - u.time.delta);
}

FlagEntity::FlagEntity(AI::Team team)
{
	create<Transform>()->absolute_pos(flag_base_pos(Team::list[team].flag_base.ref()));

	View* model = create<View>();
	model->mesh = Asset::Mesh::sphere;
	model->shader = Asset::Shader::standard;
	model->team = s8(team);
	model->offset.scale(Vec3(FLAG_RADIUS * 0.94f));
	model->color = Team::color_neutral();

	create<PlayerTrigger>()->radius = DRONE_SHIELD_RADIUS + 0.2f;

	Flag* flag = create<Flag>();
	flag->team = team;
	flag->at_base = true;
	flag->timer = FLAG_RESTORE_TIME;

	create<Target>();

	flag_build_force_field(Team::list[team].flag_base.ref(), team);
}

void glass_shatter_apply(Glass* glass, const Vec3& shatter_point, const Vec3& dir)
{
	if (glass->client_side_shatter_timer == 0.0f) // if glass has already been shattered on client side, don't do it again
	{
#if !SERVER
		Vec3 pos;
		Quat rot;
		glass->get<Transform>()->absolute(&pos, &rot);
		Vec2 center;
		Vec3 shatter_point_flattened;
		{
			Vec3 normal = rot * Vec3(0, 0, 1);
			shatter_point_flattened = shatter_point - normal * (shatter_point - pos).dot(normal);
			Vec3 center3 = rot.inverse() * (shatter_point_flattened - pos); // to local space
			center = Vec2(center3.x, center3.y);
		}
		const Vec3& size = glass->get<RigidBody>()->size;
		Vec2 corner1(-size.x, -size.y);
		Vec2 corner2(-size.x, size.y);
		Vec2 corner3(size.x, -size.y);
		Vec2 corner4(size.x, size.y);
		Vec2 a_corner1 = Vec2::lerp(0.3333f, center, corner1);
		Vec2 a_corner2 = Vec2::lerp(0.3333f, center, corner2);
		Vec2 a_corner3 = Vec2::lerp(0.3333f, center, corner3);
		Vec2 a_corner4 = Vec2::lerp(0.3333f, center, corner4);
		Vec2 b_corner1 = Vec2::lerp(0.75f, center, corner1);
		Vec2 b_corner2 = Vec2::lerp(0.75f, center, corner2);
		Vec2 b_corner3 = Vec2::lerp(0.75f, center, corner3);
		Vec2 b_corner4 = Vec2::lerp(0.75f, center, corner4);
		Vec3 impulse = dir * (2.0f / dir.length());

		GlassShard::add(center, a_corner1, a_corner2, pos, rot, shatter_point_flattened, impulse);
		GlassShard::add(a_corner1, b_corner1, a_corner2, b_corner2, pos, rot, shatter_point_flattened, impulse);
		GlassShard::add(b_corner1, corner1, b_corner2, corner2, pos, rot, shatter_point_flattened, impulse);

		GlassShard::add(center, a_corner2, a_corner4, pos, rot, shatter_point_flattened, impulse);
		GlassShard::add(a_corner2, b_corner2, a_corner4, b_corner4, pos, rot, shatter_point_flattened, impulse);
		GlassShard::add(b_corner2, corner2, b_corner4, corner4, pos, rot, shatter_point_flattened, impulse);

		GlassShard::add(center, a_corner4, a_corner3, pos, rot, shatter_point_flattened, impulse);
		GlassShard::add(a_corner4, b_corner4, a_corner3, b_corner3, pos, rot, shatter_point_flattened, impulse);
		GlassShard::add(b_corner4, corner4, b_corner3, corner3, pos, rot, shatter_point_flattened, impulse);

		GlassShard::add(center, a_corner3, a_corner1, pos, rot, shatter_point_flattened, impulse);
		GlassShard::add(a_corner3, b_corner3, a_corner1, b_corner1, pos, rot, shatter_point_flattened, impulse);
		GlassShard::add(b_corner3, corner3, b_corner1, corner1, pos, rot, shatter_point_flattened, impulse);

		Audio::post_global(AK::EVENTS::PLAY_GLASS_SHATTER, shatter_point_flattened);
#endif

		if (Game::level.local)
			World::remove(glass->entity());
		else
		{
			// client-side prediction; give time for the server to confirm the shatter
			glass->client_side_shatter_timer = 1.0f;
			glass->get<View>()->mask = 0;
			RigidBody* body = glass->get<RigidBody>();
			body->set_collision_masks(CollisionDroneIgnore, body->collision_group);
		}
	}
}

namespace GlassNet
{
	b8 shatter(Glass* glass, const Vec3& shatter_point, const Vec3& dir)
	{
		using Stream = Net::StreamWrite;
		Net::StreamWrite* p = Net::msg_new(Net::MessageType::Glass);
		{
			Ref<Glass> ref = glass;
			serialize_ref(p, ref);
		}
		Vec3 pos = shatter_point;
		if (!Net::serialize_position(p, &pos, Net::Resolution::Low))
			net_error();
		Vec3 d = dir;
		if (!Net::serialize_position(p, &d, Net::Resolution::Medium))
			net_error();
		Net::msg_finalize(p);
		return true;
	}
}

b8 Glass::net_msg(Net::StreamRead* p, Net::MessageSource src)
{
	using Stream = Net::StreamRead;
	Ref<Glass> ref;
	serialize_ref(p, ref);
	Vec3 shatter_point;
	if (!Net::serialize_position(p, &shatter_point, Net::Resolution::Low))
		net_error();
	Vec3 dir;
	if (!Net::serialize_position(p, &dir, Net::Resolution::Medium))
		net_error();
	if (Game::level.local == (src == Net::MessageSource::Loopback))
		glass_shatter_apply(ref.ref(), shatter_point, dir);
	return true;
}

void Glass::shatter(const Vec3& shatter_point, const Vec3& dir)
{
	if (Game::level.local)
		GlassNet::shatter(this, shatter_point, dir);
	else // client shatters it instantly, then waits a certain amount of time for the server to confirm
		glass_shatter_apply(this, shatter_point, dir);
}

Array<GlassShard> GlassShard::list;
AssetID GlassShard::index_buffer = AssetNull;
AssetID GlassShard::index_buffer_edges = AssetNull;

GlassShard* glass_shard_init()
{
	GlassShard* shard = GlassShard::list.add();
	new (shard) GlassShard();
	shard->timestamp = Game::time.total;
	shard->mesh_id = Loader::dynamic_mesh(1);
	Loader::dynamic_mesh_attrib(RenderDataType::Vec3);
	return shard;
}

void glass_shard_physics(const Vec3* vertices, s32 vertex_count, const Vec3& pos, const Quat& rot, r32 mass, btRigidBody** out_body, btCollisionShape** out_shape)
{
	btConvexHullShape* btShape = new btConvexHullShape((const btScalar*)(vertices), vertex_count, sizeof(Vec3));

	btVector3 localInertia(0, 0, 0);
	btShape->calculateLocalInertia(mass, localInertia);

	btRigidBody::btRigidBodyConstructionInfo info(mass, 0, btShape, localInertia);

	info.m_startWorldTransform = btTransform(rot, pos);
	btRigidBody* btBody = new btRigidBody(info);
	btBody->setWorldTransform(info.m_startWorldTransform);

	btBody->setUserIndex(-1);
	btBody->setRestitution(0.5f);
	btBody->setDamping(0.25f, 0.25f);

	Physics::btWorld->addRigidBody(btBody, CollisionDroneIgnore, CollisionStatic | CollisionElectric | CollisionParkour | CollisionInaccessible);

	*out_body = btBody;
	*out_shape = btShape;
}

GlassShard* GlassShard::add(const Vec2& a, const Vec2& b, const Vec2& c, const Vec3& pos, const Quat& rot, const Vec3& impulse_pos, const Vec3& impulse)
{
	GlassShard* shard = glass_shard_init();
	shard->rot = rot;
	Vec2 center = (a + b + c) * 0.33333333f;
	shard->pos = pos + rot * Vec3(center.x, center.y, 0);

	RenderSync* sync = Loader::swapper->get();
	sync->write(RenderOp::UpdateAttribBuffers);
	sync->write(shard->mesh_id);

	{
		const r32 gap = 0.1f;
		Vec2 a_recentered = a - center;
		a_recentered += a_recentered * (-gap / a_recentered.length());
		Vec2 b_recentered = b - center;
		b_recentered += b_recentered * (-gap / b_recentered.length());
		Vec2 c_recentered = c - center;
		c_recentered += c_recentered * (-gap / c_recentered.length());
		Vec3 vertices[] =
		{
			Vec3(a_recentered.x, a_recentered.y, 0.0f),
			Vec3(b_recentered.x, b_recentered.y, 0.0f),
			Vec3(c_recentered.x, c_recentered.y, 0.0f),
			Vec3(a_recentered.x, a_recentered.y, 0.05f),
			Vec3(b_recentered.x, b_recentered.y, 0.05f),
			Vec3(c_recentered.x, c_recentered.y, 0.05f),
		};
		r32 radius = sqrtf(vi_max(a_recentered.length_squared(), vi_max(b_recentered.length_squared(), c_recentered.length_squared())));
		glass_shard_physics(vertices, 6, shard->pos, shard->rot, radius * radius * 0.2f, &shard->btBody, &shard->btShape);
		shard->btBody->applyImpulse(impulse, (impulse_pos - shard->pos) * 0.25f);
		sync->write<s32>(3);
		sync->write<Vec3>(vertices, 3);
	}

	{
		sync->write(RenderOp::UpdateIndexBuffer);
		sync->write(shard->mesh_id);

		sync->write<s32>(3);
		s32 indices[3] = { 0, 1, 2 };
		sync->write<s32>(indices, 3);
	}

	{
		sync->write(RenderOp::UpdateEdgesIndexBuffer);
		sync->write(shard->mesh_id);

		sync->write<s32>(6);
		s32 indices[6] =
		{
			0, 1,
			1, 2, 
			2, 1,
		};
		sync->write<s32>(indices, 6);
	}

	return shard;
}

GlassShard* GlassShard::add(const Vec2& a, const Vec2& b, const Vec2& c, const Vec2& d, const Vec3& pos, const Quat& rot, const Vec3& impulse_pos, const Vec3& impulse)
{
	GlassShard* shard = glass_shard_init();
	shard->rot = rot;
	Vec2 center = (a + b + c + d) * 0.25f;
	shard->pos = pos + rot * Vec3(center.x, center.y, 0);

	RenderSync* sync = Loader::swapper->get();
	sync->write(RenderOp::UpdateAttribBuffers);
	sync->write(shard->mesh_id);

	{
		const r32 gap = 0.1f;
		Vec2 a_recentered = a - center;
		a_recentered += a_recentered * (-gap / a_recentered.length());
		Vec2 b_recentered = b - center;
		b_recentered += b_recentered * (-gap / b_recentered.length());
		Vec2 c_recentered = c - center;
		c_recentered += c_recentered * (-gap / c_recentered.length());
		Vec2 d_recentered = d - center;
		d_recentered += d_recentered * (-gap / d_recentered.length());
		Vec3 vertices[] =
		{
			Vec3(a_recentered.x, a_recentered.y, 0.0f),
			Vec3(b_recentered.x, b_recentered.y, 0.0f),
			Vec3(c_recentered.x, c_recentered.y, 0.0f),
			Vec3(d_recentered.x, d_recentered.y, 0.0f),
			Vec3(a_recentered.x, a_recentered.y, 0.05f),
			Vec3(b_recentered.x, b_recentered.y, 0.05f),
			Vec3(c_recentered.x, c_recentered.y, 0.05f),
			Vec3(d_recentered.x, d_recentered.y, 0.05f),
		};
		r32 radius = sqrtf(vi_max(a_recentered.length_squared(), vi_max(b_recentered.length_squared(), vi_max(c_recentered.length_squared(), d_recentered.length_squared()))));
		glass_shard_physics(vertices, 8, shard->pos, shard->rot, radius * radius * 0.2f, &shard->btBody, &shard->btShape);
		shard->btBody->applyImpulse(impulse, (impulse_pos - shard->pos) * 0.25f);
		sync->write<s32>(4);
		sync->write<Vec3>(vertices, 4);
	}

	{
		sync->write(RenderOp::UpdateIndexBuffer);
		sync->write(shard->mesh_id);

		sync->write<s32>(6);
		s32 indices[6] = { 0, 1, 2, 1, 3, 2 };
		sync->write<s32>(indices, 6);
	}

	{
		sync->write(RenderOp::UpdateEdgesIndexBuffer);
		sync->write(shard->mesh_id);

		sync->write<s32>(8);
		s32 indices[8] =
		{
			0, 1,
			1, 3,
			3, 2,
			2, 0,
		};
		sync->write<s32>(indices, 8);
	}

	return shard;
}

void GlassShard::clear()
{
	// dynamic meshes are temporary; they'll automatically be freed
	for (s32 i = 0; i < list.length; i++)
		list[i].cleanup();
	list.length = 0;
}

void GlassShard::cleanup()
{
	Loader::dynamic_mesh_free(mesh_id);
	Physics::btWorld->removeRigidBody(btBody);
	delete btBody;
	delete btShape;
}

void GlassShard::sync_physics()
{
	for (s32 i = 0; i < list.length; i++)
	{
		GlassShard* s = &list[i];
		btTransform transform = s->btBody->getInterpolationWorldTransform();
		s->pos = transform.getOrigin();
		s->rot = transform.getRotation();
	}
}

#define GLASS_SHARD_LIFETIME 5.0f
void GlassShard::update_all(const Update& u)
{
	for (s32 i = 0; i < list.length; i++)
	{
		GlassShard* shard = &list[i];
		if (u.time.total - shard->timestamp > GLASS_SHARD_LIFETIME)
		{
			shard->cleanup();
			list.remove(i);
			i--;
		}
	}
}

void GlassShard::draw_all(const RenderParams& params)
{
	Loader::shader(Asset::Shader::flat);

	RenderSync* sync = params.sync;
	sync->write(RenderOp::Shader);
	sync->write(Asset::Shader::flat);
	sync->write(params.technique);

	Mat4 m;
	for (s32 i = 0; i < list.length; i++)
	{
		const GlassShard& shard = list[i];
		m.make_transform(shard.pos, Vec3(1), shard.rot);

		sync->write(RenderOp::Uniform);
		sync->write(Asset::Uniform::mvp);
		sync->write(RenderDataType::Mat4);
		sync->write<s32>(1);
		sync->write<Mat4>(m * params.view_projection);

		sync->write(RenderOp::Uniform);
		sync->write(Asset::Uniform::mv);
		sync->write(RenderDataType::Mat4);
		sync->write<s32>(1);
		sync->write<Mat4>(m * params.view);

		sync->write(RenderOp::Uniform);
		sync->write(Asset::Uniform::diffuse_color);
		sync->write(RenderDataType::Vec4);
		sync->write<s32>(1);
		r32 lifetime = Game::time.total - shard.timestamp;
		r32 blend = 1.0f - (lifetime / GLASS_SHARD_LIFETIME);
		blend = vi_min(1.0f, blend / 0.2f);
		sync->write<Vec4>(Vec4(1, 1, 1, 0.5f * blend));

		if (params.flags & RenderFlagEdges)
		{
			sync->write(RenderOp::MeshEdges);
			sync->write(shard.mesh_id);
		}
		else
		{
			sync->write(RenderOp::Mesh);
			sync->write(RenderPrimitiveMode::Triangles);
			sync->write(shard.mesh_id);
		}
	}
}

void Glass::shatter_all(const Vec3& start, const Vec3& end)
{
	btCollisionWorld::AllHitsRayResultCallback ray_callback(start, end);
	Physics::raycast(&ray_callback, CollisionGlass);
	for (s32 i = 0; i < ray_callback.m_collisionObjects.size(); i++)
	{
		Entity* entity = &Entity::list[ray_callback.m_collisionObjects[i]->getUserIndex()];
		entity->get<Glass>()->shatter(ray_callback.m_hitPointWorld[i], end - start);
	}
}

void Glass::update_client(const Update& u)
{
	if (client_side_shatter_timer > 0.0f)
	{
		client_side_shatter_timer = vi_max(0.0f, client_side_shatter_timer - u.time.delta);
		if (client_side_shatter_timer == 0.0f)
		{
			// server never confirmed shatter; reset
			RigidBody* body = get<RigidBody>();
			body->set_collision_masks(CollisionGlass, body->collision_group);
			get<View>()->mask = RENDER_MASK_DEFAULT;
		}
	}
}

GlassEntity::GlassEntity(const Vec2& size)
{
	create<Transform>();

	create<RigidBody>(RigidBody::Type::Box, Vec3(size.x, size.y, 0.1f), 0.0f, CollisionGlass, ~CollisionStatic & ~CollisionAudio & ~CollisionParkour & ~CollisionInaccessible & ~CollisionElectric & ~CollisionGlass & ~CollisionAllTeamsForceField);

	View* view = create<View>();
	view->mesh = Asset::Mesh::glass_pane;
	view->shader = Asset::Shader::flat;
	view->alpha();
	view->offset = Mat4::make_scale(Vec3(size.x, size.y, 1.0f));

	create<Glass>();
}

MinionSpawnerEntity::MinionSpawnerEntity(PlayerManager* owner, AI::Team team, const Vec3& abs_pos, const Quat& abs_rot, Transform* parent)
{
	{
		Transform* transform = create<Transform>();
		if (parent && parent->has<Minion>())
			transform->parent = parent; // parent at 0, 0, 0
		else
		{
			transform->pos = abs_pos;
			transform->rot = abs_rot;
			transform->reparent(parent);
		}
	}

	create<Health>(MINION_SPAWNER_HEALTH, MINION_SPAWNER_HEALTH, DRONE_SHIELD_AMOUNT, DRONE_SHIELD_AMOUNT);

	View* model = create<View>();
	model->color = Team::color_neutral();
	model->mesh = Asset::Mesh::minion_spawner_main;
	model->shader = Asset::Shader::culled;
	model->team = s8(team);
	if (parent && parent->has<Minion>())
		model->mesh = Asset::Mesh::minion_spawner_attached;

	create<Target>();

	{
		MinionSpawner* spawner = create<MinionSpawner>();
		spawner->owner = owner;
		spawner->team = team;
	}

	PointLight* light = create<PointLight>();
	light->color = Team::color_neutral().xyz();
	light->team = s8(team);
	light->type = PointLight::Type::Normal;
	light->radius = TURRET_RANGE * 0.5f;

	create<Shield>();
}

void MinionSpawner::awake()
{
	if (Transform* parent = get<Transform>()->parent.ref())
	{
		if (parent->has<Minion>())
			parent->get<Minion>()->carrying = entity();
	}
	link_arg<Entity*, &MinionSpawner::killed_by>(get<Health>()->killed);
}

void MinionSpawner::killed_by(Entity* e)
{
	// let everyone know what happened
	PlayerHuman::notification(get<Transform>()->absolute_pos(), team, PlayerHuman::Notification::Type::MinionSpawnerDestroyed);
	PlayerHuman::log_add(_(strings::minion_spawner_destroyed), AI::TeamNone, 1 << team);
	PlayerManager::entity_killed_by(entity(), e);

	if (Game::level.local)
	{
		Vec3 pos;
		Quat rot;
		get<Transform>()->absolute(&pos, &rot);
		ParticleEffect::spawn(ParticleEffect::Type::Explosion, pos, rot);
		World::remove_deferred(entity());
	}
}

TurretEntity::TurretEntity(PlayerManager* owner, AI::Team team, const Vec3& abs_pos, const Quat& abs_rot, Transform* parent)
{
	{
		Transform* transform = create<Transform>();
		if (parent && parent->has<Minion>())
			transform->parent = parent; // parent at 0, 0, 0
		else
		{
			transform->pos = abs_pos;
			transform->rot = abs_rot;
			transform->reparent(parent);
		}
	}
	create<Audio>();

	View* view = create<View>();
	view->mesh = Asset::Mesh::turret;
	view->shader = Asset::Shader::culled;
	view->team = s8(team);
	view->color = Team::color_neutral();
	
	{
		Turret* turret = create<Turret>();
		turret->team = team;
		turret->owner = owner;
	}

	create<Target>();

	create<Health>(TURRET_HEALTH, TURRET_HEALTH, DRONE_SHIELD_AMOUNT, DRONE_SHIELD_AMOUNT);
	create<Shield>();

	PointLight* light = create<PointLight>();
	light->color = Team::color_neutral().xyz();
	light->team = s8(team);
	light->type = PointLight::Type::Normal;
	light->radius = TURRET_RANGE * 0.5f;
}

void Turret::awake()
{
	if (Transform* parent = get<Transform>()->parent.ref())
	{
		if (parent->has<Minion>())
			parent->get<Minion>()->carrying = entity();
	}
	target_check_time = mersenne::randf_oo() * TURRET_TARGET_CHECK_TIME;
	link_arg<Entity*, &Turret::killed>(get<Health>()->killed);
}

Turret::~Turret()
{
	get<Audio>()->stop_all();
}

void Turret::killed(Entity* by)
{
	PlayerManager::entity_killed_by(entity(), by);
	PlayerHuman::notification(get<Transform>()->absolute_pos(), team, PlayerHuman::Notification::Type::TurretDestroyed);
	PlayerHuman::log_add(_(strings::turret_destroyed), AI::TeamNone, 1 << team);

	if (Game::level.local)
	{
		Vec3 pos;
		Quat rot;
		get<Transform>()->absolute(&pos, &rot);
		ParticleEffect::spawn(ParticleEffect::Type::Explosion, pos, rot);
		World::remove_deferred(entity());
	}
}

namespace TurretNet
{
	b8 update_target(Turret* turret, Entity* target)
	{
		using Stream = Net::StreamWrite;
		Net::StreamWrite* p = Net::msg_new(Net::MessageType::Turret);
		{
			Ref<Turret> ref_turret = turret;
			serialize_ref(p, ref_turret);
		}
		{
			Ref<Entity> ref_target = target;
			serialize_ref(p, ref_target);
		}
		Net::msg_finalize(p);
		return true;
	}
}

b8 Turret::net_msg(Net::StreamRead* p, Net::MessageSource src)
{
	using Stream = Net::StreamRead;
	Ref<Turret> ref;
	serialize_ref(p, ref);
	Ref<Entity> target;
	serialize_ref(p, target);
	if (Game::level.local == (src == Net::MessageSource::Loopback))
		ref.ref()->target = target;
	return true;
}

b8 Turret::can_see(Entity* target) const
{
	Vec3 pos = get<Transform>()->absolute_pos();

	Vec3 target_pos = target->has<Target>() ? target->get<Target>()->absolute_pos() : target->get<Transform>()->absolute_pos();

	if (!target->has<ForceField>() && ForceField::hash(team, pos) != ForceField::hash(team, target_pos))
		return false;

	if (target->has<Minion>() && target->get<Minion>()->carrying.ref() == entity()) // don't attack the minion if it's carrying us
		return false;

	Vec3 to_target = target_pos - pos;
	float distance_to_target = to_target.length();

	if (target->has<ForceField>())
		distance_to_target -= FORCE_FIELD_RADIUS;

	if (distance_to_target < TURRET_RANGE)
	{
		RaycastCallbackExcept ray_callback(pos, target_pos, entity());
		Physics::raycast(&ray_callback, (CollisionStatic | CollisionInaccessible | CollisionElectric | CollisionAllTeamsForceField) & ~Team::force_field_mask(team));
		if (ray_callback.hasHit())
		{
			Entity* hit = &Entity::list[ray_callback.m_collisionObject->getUserIndex()];
			if (hit->has<ForceFieldCollision>())
				hit = hit->get<ForceFieldCollision>()->field.ref()->entity();
			return hit == target;
		}
		else
			return true;
	}
	return false;
}

s32 turret_priority(Entity* e)
{
	if (e->has<Battery>() || e->has<SpawnPoint>())
		return -1; // never attack
	else if (e->has<Minion>())
		return 2;
	else
		return 1;
}

void Turret::check_target()
{
	// if we are targeting an enemy
	// make sure we still want to do that
	if (target.ref() && can_see(target.ref()))
		return;

	// find a new target
	Ref<Entity> target_old = target;

	Ref<Entity> target_new = nullptr;

	s32 target_priority = 0;
	for (auto i = Health::list.iterator(); !i.is_last(); i.next())
	{
		Entity* e = i.item()->entity();
		AI::Team e_team = AI::entity_team(e);
		if (e_team != AI::TeamNone
			&& e_team != team
			&& can_see(i.item()->entity()))
		{
			s32 candidate_priority = turret_priority(i.item()->entity());
			if (candidate_priority > target_priority)
			{
				target_new = i.item()->entity();
				target_priority = candidate_priority;
			}
		}
	}

	if (!target_new.equals(target_old))
		TurretNet::update_target(this, target_new.ref());
}

void Turret::update_server(const Update& u)
{
	target_check_time -= u.time.delta;
	if (target_check_time < 0.0f)
	{
		target_check_time += TURRET_TARGET_CHECK_TIME;
		check_target();
	}

	cooldown = vi_max(0.0f, cooldown - u.time.delta);

	if (cooldown == 0.0f && target.ref())
	{
		if (can_see(target.ref()))
		{
			Vec3 gun_pos = get<Transform>()->absolute_pos();
			Vec3 aim_pos;
			if (!target.ref()->has<Target>() || !target.ref()->get<Target>()->predict_intersection(gun_pos, BOLT_SPEED_TURRET, nullptr, &aim_pos))
				aim_pos = target.ref()->get<Transform>()->absolute_pos();
			gun_pos += Vec3::normalize(aim_pos - gun_pos) * TURRET_RADIUS;
			Net::finalize(World::create<BoltEntity>(team, nullptr, entity(), Bolt::Type::Turret, gun_pos, aim_pos - gun_pos));
			cooldown += TURRET_COOLDOWN;
		}
		else
			target_check_time = 0.0f;
	}
}

r32 Turret::particle_accumulator;
void Turret::update_all(const Update& u)
{
	for (auto i = list.iterator(); !i.is_last(); i.next())
	{
		if (Game::level.local)
			i.item()->update_server(u);
		if (!entity_minion_attach_update(i.item()->get<Transform>(), &i.item()->abs_pos_attached, Asset::Mesh::turret, TURRET_RADIUS, TURRET_RADIUS))
			continue;
	}

	const r32 interval = 0.05f;
	particle_accumulator += u.time.delta;
	while (particle_accumulator > interval)
	{
		particle_accumulator -= interval;
		for (auto i = list.iterator(); !i.is_last(); i.next())
		{
			Entity* target = i.item()->target.ref();
			if (target)
			{
				// spawn particle effect
				Vec3 offset = Quat::euler(0.0f, mersenne::randf_co() * PI * 2.0f, (mersenne::randf_co() - 0.5f) * PI) * Vec3(0, 0, 1.5f);
				Particles::fast_tracers.add
				(
					i.item()->get<Transform>()->absolute_pos() + offset,
					offset * -5.0f,
					0
				);
			}

			i.item()->get<Audio>()->param(AK::GAME_PARAMETERS::TURRET_CHARGE, target && target->has<PlayerControlHuman>() && target->get<PlayerControlHuman>()->local() ? 1.0f : 0.0f);

			if (b8(target) != i.item()->charging)
			{
				if (target)
					i.item()->get<Audio>()->post(AK::EVENTS::PLAY_TURRET_CHARGE);
				else
					i.item()->get<Audio>()->stop(AK::EVENTS::STOP_TURRET_CHARGE);
				i.item()->charging = b8(target);
			}
		}
	}
}

// returns true if the given position is inside an enemy force field
ForceField* ForceField::inside(AI::TeamMask mask, const Vec3& pos, r32 extra_radius)
{
	for (auto i = list.iterator(); !i.is_last(); i.next())
	{
		if (AI::match(i.item()->team, mask) && i.item()->contains(pos, extra_radius))
			return i.item();
	}
	return nullptr;
}

b8 ForceField::contains(const Vec3& pos, r32 extra_radius) const
{
	return (pos - get<Transform>()->absolute_pos()).length_squared() < (FORCE_FIELD_RADIUS + extra_radius) * (FORCE_FIELD_RADIUS + extra_radius);
}

b8 ForceField::is_flag_force_field() const
{
	return contains(Team::list[team].flag_base.ref()->absolute_pos());
}

// describes which enemy force fields you are currently inside
u32 ForceField::hash(AI::Team my_team, const Vec3& pos, HashMode mode)
{
	u32 result = 0;
	for (auto i = list.iterator(); !i.is_last(); i.next())
	{
		if (i.item()->team != my_team
			&& (mode == HashMode::All || (i.item()->flags & FlagInvincible))
			&& (pos - i.item()->get<Transform>()->absolute_pos()).length_squared() < FORCE_FIELD_RADIUS * FORCE_FIELD_RADIUS)
		{
			if (result == 0)
				result = 1;
			result += MAX_ENTITIES % (i.index + 37); // todo: learn how to math
		}
	}
	return result;
}

#define FORCE_FIELD_ANIM_TIME 0.4f
#define FORCE_FIELD_DAMAGE_TIME 0.15f
#define FORCE_FIELD_BASE_RADIUS 0.385f

ForceField::ForceField()
	: team(AI::TeamNone),
	spawn_death_timer(FORCE_FIELD_ANIM_TIME),
	damage_timer(),
	flags(),
	collision()
{
}

void ForceField::awake()
{
	if (Transform* parent = get<Transform>()->parent.ref())
	{
		if (parent->has<Minion>())
			parent->get<Minion>()->carrying = entity();
	}
	if (!(flags & FlagPermanent))
	{
		link_arg<Entity*, &ForceField::killed>(get<Health>()->killed);
		link_arg<const HealthEvent&, &ForceField::health_changed>(get<Health>()->changed);
	}
	get<Audio>()->entry()->flag(AudioEntry::FlagEnableForceFieldObstruction, false);
	get<Audio>()->post(AK::EVENTS::PLAY_FORCE_FIELD_LOOP);
}

ForceField::~ForceField()
{
	if (Game::level.local && collision.ref())
		World::remove_deferred(collision.ref()->entity());

	get<Audio>()->stop_all();
}

Vec3 ForceField::base_pos() const
{
	return get<Transform>()->to_world(Vec3(0, 0, -FORCE_FIELD_BASE_OFFSET));
}

void ForceField::health_changed(const HealthEvent& e)
{
	if (e.hp + e.shield < 0)
	{
		PlayerManager* rewardee = PlayerManager::owner(e.source.ref());
		if (rewardee && rewardee->team.ref()->team() != team)
			rewardee->add_energy_and_notify(-s32(e.hp + e.shield));
		damage_timer = FORCE_FIELD_DAMAGE_TIME;
		if (PlayerHuman::notification(entity(), team, PlayerHuman::Notification::Type::ForceFieldUnderAttack))
			PlayerHuman::log_add(_(strings::force_field_under_attack), AI::TeamNone, 1 << team);
	}
}

void ForceField::killed(Entity* e)
{
	if (e)
	{
		PlayerHuman::notification(get<Transform>()->absolute_pos(), team, PlayerHuman::Notification::Type::ForceFieldDestroyed);
		PlayerHuman::log_add(_(strings::force_field_destroyed), AI::TeamNone, 1 << team);
		PlayerManager::entity_killed_by(entity(), e);
	}

	if (has<View>())
	{
		if (Game::level.local)
		{
			Vec3 pos;
			Quat rot;
			get<Transform>()->absolute(&pos, &rot);
			ParticleEffect::spawn(ParticleEffect::Type::Explosion, pos, rot);
		}

		get<View>()->mask = 0;
	}
	spawn_death_timer = FORCE_FIELD_ANIM_TIME;
}

ForceField* ForceField::closest(AI::TeamMask mask, const Vec3& pos, r32* distance)
{
	ForceField* closest = nullptr;
	r32 closest_distance = FLT_MAX;

	for (auto i = list.iterator(); !i.is_last(); i.next())
	{
		if (AI::match(i.item()->team, mask))
		{
			r32 d = (i.item()->get<Transform>()->absolute_pos() - pos).length_squared();
			if (d < closest_distance)
			{
				closest = i.item();
				closest_distance = d;
			}
		}
	}

	if (distance)
	{
		if (closest)
			*distance = sqrtf(closest_distance);
		else
			*distance = FLT_MAX;
	}

	return closest;
}

r32 ForceField::particle_accumulator;
void ForceField::update_all(const Update& u)
{
	const r32 interval = 0.1f;
	particle_accumulator += u.time.delta;
	while (particle_accumulator > interval)
	{
		particle_accumulator -= interval;
		for (auto i = list.iterator(); !i.is_last(); i.next())
		{
			if (!(i.item()->flags & FlagInvincible))
			{
				Vec3 pos = i.item()->get<Transform>()->absolute_pos();

				// spawn particle effect
				Particles::eased_particles.add
				(
					pos + Quat::euler(0.0f, mersenne::randf_co() * PI * 2.0f, (mersenne::randf_co() - 0.5f) * PI) * Vec3(0, 0, 2.0f),
					pos,
					0
				);
			}
		}
	}

	for (auto i = list.iterator(); !i.is_last(); i.next())
	{
		if (i.item()->flags & FlagAttached)
		{
			if (!entity_minion_attach_update(i.item()->get<Transform>(), &i.item()->abs_pos_attached, Asset::Mesh::force_field_base, FORCE_FIELD_BASE_RADIUS, FORCE_FIELD_BASE_OFFSET))
				continue;
		}

		View* v = i.item()->collision.ref()->get<View>();

		if (i.item()->spawn_death_timer > 0.0f)
		{
			i.item()->spawn_death_timer = vi_max(0.0f, i.item()->spawn_death_timer - u.time.delta);

			r32 blend = 1.0f - (i.item()->spawn_death_timer / FORCE_FIELD_ANIM_TIME);

			s8 hp = (i.item()->flags & FlagPermanent) ? 1 : i.item()->get<Health>()->hp;
			if (hp == 0)
			{
				// dying
				v->color.w = 0.65f * (1.0f - blend);
				v->offset = Mat4::make_scale(Vec3(1.0f + Ease::cubic_in<r32>(blend) * 0.25f));
			}
			else
			{
				// spawning
				v->color.w = 0.35f * blend;
				v->offset = Mat4::make_scale(Vec3(Ease::cubic_out<r32>(blend)));
			}

			if (Game::level.local && blend == 1.0f && hp == 0)
				World::remove_deferred(i.item()->entity());
		}
		else
		{
			if (Game::level.local) // check for collisions
			{
				Vec3 me = i.item()->get<Transform>()->absolute_pos();
				b8 attached = i.item()->flags & FlagAttached;
				AI::Team my_team = i.item()->team;
				auto j = i;
				if (!j.is_last())
					j.next(); // don't need to check against self
				for (; !j.is_last(); j.next())
				{
					if (j.item()->team == my_team && (attached || (j.item()->flags & FlagAttached)) && j.item()->spawn_death_timer == 0.0f)
					{
						Vec3 them = j.item()->get<Transform>()->absolute_pos();
						if ((me - them).length_squared() < FORCE_FIELD_RADIUS * FORCE_FIELD_RADIUS)
						{
							ParticleEffect::spawn(ParticleEffect::Type::ImpactLarge, (me + them) * 0.5f, Quat::look(Vec3(0, 1, 0)));
							if (attached)
							{
								i.item()->get<Health>()->kill(nullptr);
								break;
							}
							else
								j.item()->get<Health>()->kill(nullptr);
						}
					}
				}
			}

			// damage animation
			if (i.item()->damage_timer > 0.0f)
			{
				i.item()->damage_timer = vi_max(0.0f, i.item()->damage_timer - u.time.delta);
				v->color.w = 0.35f + (i.item()->damage_timer / FORCE_FIELD_DAMAGE_TIME) * 0.65f;
			}
		}
	}
}

void ForceField::destroy()
{
	vi_assert(Game::level.local);
	get<Health>()->kill(nullptr);
}

b8 ForceField::can_spawn(AI::Team team, const Vec3& abs_pos, r32 radius)
{
	// can't be near an invincible force field
	for (auto i = ForceField::list.iterator(); !i.is_last(); i.next())
	{
		if (i.item()->team == team
			&& (i.item()->get<Transform>()->absolute_pos() - abs_pos).length_squared() < (FORCE_FIELD_RADIUS + radius) * (FORCE_FIELD_RADIUS + radius)
			&& (i.item()->flags & ForceField::FlagInvincible))
			return false;
	}
	return true;
}

ForceFieldEntity::ForceFieldEntity(Transform* parent, const Vec3& abs_pos, const Quat& abs_rot, AI::Team team, ForceField::Type type)
{
	Transform* transform = create<Transform>();
	if (parent && parent->has<Minion>())
		transform->parent = parent; // parent at 0, 0, 0
	else
	{
		transform->pos = abs_pos;
		transform->rot = abs_rot;
		transform->reparent(parent);
	}

	create<Audio>();

	ForceField* field = create<ForceField>();
	field->team = team;

	if (type == ForceField::Type::Battery)
	{
		field->flags |= ForceField::FlagBattery;

		// kill all enemy things inside
		for (auto i = Health::list.iterator(); !i.is_last(); i.next())
		{
			if (!i.item()->has<Battery>())
			{
				AI::Team other_team = AI::entity_team(i.item()->entity());
				if (other_team != AI::TeamNone
					&& other_team != team
					&& (i.item()->get<Transform>()->absolute_pos() - abs_pos).length_squared() < (FORCE_FIELD_RADIUS + DRONE_RADIUS) * (FORCE_FIELD_RADIUS + DRONE_RADIUS))
				{
					i.item()->kill(nullptr);
				}
			}
		}
	}

	if (type == ForceField::Type::Permanent)
		field->flags |= ForceField::FlagPermanent | ForceField::FlagInvincible;
	else
	{
		// normal (non-permanent) force field

		{
			s8 health;
			{
				s32 enemy_count = PlayerManager::count_team_mask(~(1 << team));
				if (enemy_count <= 1)
					health = FORCE_FIELD_HEALTH_NERFED;
				else if (enemy_count <= 2)
					health = FORCE_FIELD_HEALTH_NORMAL;
				else
					health = FORCE_FIELD_HEALTH_BUFFED;
			}
			create<Health>(health, health);
		}
		create<Target>();
		create<Shield>();

		if (type == ForceField::Type::Invincible)
			field->flags |= ForceField::FlagInvincible;
		else if (type == ForceField::Type::Normal || type == ForceField::Type::Battery)
		{
			// destroy any overlapping friendly force field
			for (auto i = ForceField::list.iterator(); !i.is_last(); i.next())
			{
				// if we're spawning right on top of an enemy force field, destroy it.
				// this should only happen with battery force fields.
				// normally it's impossible to spawn a force field inside an enemy force field.
				r32 radius = (i.item()->team == team)
					? FORCE_FIELD_RADIUS * 2.0f // friendly force fields can't touch at all
					: FORCE_FIELD_RADIUS * 0.75f; // enemy force fields shouldn't spawn right on top of each other
				if (i.item() != field
					&& !(i.item()->flags & ForceField::FlagInvincible)
					&& (i.item()->get<Transform>()->absolute_pos() - abs_pos).length_squared() < radius * radius)
				{
					i.item()->destroy();
				}
			}

			View* model = create<View>();
			model->color = Team::color_neutral();
			model->team = team;
			model->mesh = Asset::Mesh::force_field_base;
			model->shader = Asset::Shader::standard;

			if (parent && parent->has<Minion>())
			{
				field->flags |= ForceField::FlagAttached;
				model->mesh = Asset::Mesh::force_field_base_attached;
			}
		}
	}

	// collision
	{
		Entity* f = World::create<Empty>();
		f->get<Transform>()->parent = transform;
		ForceFieldCollision* collision = f->add<ForceFieldCollision>();
		collision->field = field;

		View* view = f->add<View>();
		view->team = s8(team);
		view->mesh = Asset::Mesh::force_field_sphere;
		view->shader = Asset::Shader::fresnel;
		view->alpha();
		view->color.w = 0.35f;

		{
			CollisionGroup team_group = CollisionGroup(1 << (8 + team));
			f->add<RigidBody>(RigidBody::Type::Mesh, Vec3::zero, 0.0f, team_group, CollisionTarget, view->mesh);
		}

		Net::finalize_child(f);

		field->collision = collision;
	}
}

r32 Bolt::speed(Type t, b8 reflected)
{
	if (t == Type::DroneBolter)
		return BOLT_SPEED_DRONE_BOLTER;
	else if (t == Type::DroneShotgun)
		return BOLT_SPEED_DRONE_SHOTGUN;
	else if (t == Type::Turret)
		return BOLT_SPEED_TURRET * (reflected ? 2.0f : 1.0f);
	else
	{
		vi_assert(t == Type::Minion);
		return BOLT_SPEED_MINION * (reflected ? 2.0f : 1.0f);
	}
}

#define TELEPORTER_RADIUS 0.5f
#define BOLT_THICKNESS 0.05f
BoltEntity::BoltEntity(AI::Team team, PlayerManager* player, Entity* owner, Bolt::Type type, const Vec3& abs_pos, const Vec3& velocity)
{
	Vec3 dir = Vec3::normalize(velocity);
	Transform* transform = create<Transform>();
	transform->absolute_pos(abs_pos);
	transform->absolute_rot(Quat::look(dir));

	if (type != Bolt::Type::DroneShotgun)
	{
		PointLight* light = create<PointLight>();
		light->radius = BOLT_LIGHT_RADIUS;
		light->color = Team::color_neutral().xyz();
	}

	r32 speed = Bolt::speed(type);

	Bolt* b = create<Bolt>();
	r32 range = type == Bolt::Type::DroneBolter ? (DRONE_MAX_DISTANCE * 2.0f) : DRONE_MAX_DISTANCE;
	b->remaining_lifetime = (range * 0.99f) / speed;
	b->team = team;
	b->owner = owner;
	b->player = player;
	b->velocity = dir * speed;
	b->type = type;
}

namespace BoltNet
{
	b8 reflect(Bolt* b)
	{
		using Stream = Net::StreamWrite;
		Net::StreamWrite* p = Net::msg_new(Net::MessageType::Bolt);
		{
			Ref<Bolt> ref = b;
			serialize_ref(p, ref);
		}
		b8 change_team = false;
		serialize_bool(p, change_team);
		Net::msg_finalize(p);
		return true;
	}

	b8 reflect(Bolt* b, AI::Team team, const PlayerManager* player, const Entity* owner)
	{
		using Stream = Net::StreamWrite;
		Net::StreamWrite* p = Net::msg_new(Net::MessageType::Bolt);
		{
			Ref<Bolt> ref = b;
			serialize_ref(p, ref);
		}
		b8 change_team = true;
		serialize_bool(p, change_team);
		serialize_s8(p, team);
		{
			Ref<PlayerManager> ref = (PlayerManager*)player;
			serialize_ref(p, ref);
		}
		{
			Ref<Entity> ref = (Entity*)(owner);
			serialize_ref(p, ref);
		}
		Net::msg_finalize(p);
		return true;
	}
}

b8 Bolt::net_msg(Net::StreamRead* p, Net::MessageSource src)
{
	using Stream = Net::StreamRead;

	Ref<Bolt> ref;
	b8 change_team;
	AI::Team team;
	Ref<PlayerManager> player;
	Ref<Entity> owner;

	serialize_ref(p, ref);
	serialize_bool(p, change_team);
	if (change_team)
	{
		serialize_s8(p, team);
		serialize_ref(p, player);
		serialize_ref(p, owner);
	}

	if (ref.ref())
	{
		if (ref.ref()->type != Type::DroneShotgun || mersenne::rand() % 2 == 0)
			Audio::post_global(AK::EVENTS::PLAY_BOLT_REFLECT, ref.ref()->get<Transform>()->absolute_pos());
		if (change_team)
		{
			ref.ref()->reflected = true;
			ref.ref()->team = team;
			ref.ref()->player = player;
			ref.ref()->owner = owner;
		}
	}
	return true;
}

void Bolt::awake()
{
	last_pos = get<Transform>()->absolute_pos();

	if (owner.ref() && owner.ref()->has<Turret>())
	{
		owner.ref()->get<Audio>()->post(AK::EVENTS::PLAY_BOLT_SPAWN); // HACK
		EffectLight::add(last_pos, DRONE_RADIUS * 1.5f, 0.1f, EffectLight::Type::MuzzleFlash);
	}
}

b8 Bolt::visible() const
{
	return velocity.length_squared() > 0.0f;
}

b8 Bolt::default_raycast_filter(Entity* e, AI::Team team)
{
	return (!e->has<AIAgent>() || e->get<AIAgent>()->team != team) // ignore friendlies
		&& (!e->has<Rectifier>() || e->get<Rectifier>()->team != team) // ignore friendly rectifiers
		&& (!e->has<Drone>() || !UpgradeStation::drone_inside(e->get<Drone>())) // ignore drones inside upgrade stations
		&& (!e->has<ForceField>() || e->get<ForceField>()->team != team) // ignore friendly force fields
		&& (!e->has<MinionSpawner>() || e->get<MinionSpawner>()->team != team) // ignore friendly minion spawners
		&& (!e->has<Turret>() || e->get<Turret>()->team != team); // ignore friendly turrets
}

b8 Bolt::raycast(const Vec3& trace_start, const Vec3& trace_end, s16 mask, AI::Team team, Hit* out_hit, b8(*filter)(Entity*, AI::Team), const Net::StateFrame* state_frame, r32 extra_radius)
{
	out_hit->entity = nullptr;
	r32 closest_hit_distance_sq = FLT_MAX;

	{
		btCollisionWorld::ClosestRayResultCallback ray_callback(trace_start, trace_end);
		Physics::raycast(&ray_callback, mask);
		if (ray_callback.hasHit())
		{
			out_hit->point = ray_callback.m_hitPointWorld;
			out_hit->normal = ray_callback.m_hitNormalWorld;
			out_hit->entity = &Entity::list[ray_callback.m_collisionObject->getUserIndex()];
			closest_hit_distance_sq = (out_hit->point - trace_start).length_squared();
		}
	}

	// check target collisions
	for (auto i = Target::list.iterator(); !i.is_last(); i.next())
	{
		if (!filter(i.item()->entity(), team))
			continue;

		Vec3 p;
		if (state_frame)
		{
			Vec3 pos;
			Quat rot;
			Vec3 local_offset;
			Net::transform_absolute(*state_frame, i.item()->get<Transform>()->id(), &pos, &rot, &local_offset);
			p = pos + (rot * local_offset);
		}
		else
			p = i.item()->absolute_pos();

		Vec3 intersection;
		if (LMath::ray_sphere_intersect(trace_start, trace_end, p, i.item()->radius() + extra_radius, &intersection))
		{
			r32 distance_sq = (intersection - trace_start).length_squared();
			if (distance_sq < closest_hit_distance_sq)
			{
				out_hit->point = intersection;
				out_hit->normal = Vec3::normalize(intersection - p);
				out_hit->entity = i.item()->entity();
				closest_hit_distance_sq = distance_sq;
			}
		}
	}

	return out_hit->entity;
}

// returns true if the bolt hit something
b8 Bolt::simulate(r32 dt, Hit* out_hit, const Net::StateFrame* state_frame)
{
	remaining_lifetime -= dt;
	if (!state_frame && remaining_lifetime < 0.0f)
	{
		if (visible())
			ParticleEffect::spawn(ParticleEffect::Type::Fizzle, get<Transform>()->absolute_pos(), Quat::look(Vec3::normalize(velocity)));
		World::remove_deferred(entity());
		return false;
	}

	if (!visible()) // waiting for damage buffer
		return false;

	Vec3 pos = get<Transform>()->absolute_pos();
	Vec3 next_pos = pos + velocity * dt;
	Vec3 trace_end = next_pos + Vec3::normalize(velocity) * BOLT_LENGTH;

	Glass::shatter_all(pos, trace_end);

	Hit hit;
	if (raycast(pos, trace_end, CollisionStatic | (CollisionAllTeamsForceField & Bolt::raycast_mask(team)), team, &hit, &default_raycast_filter, state_frame))
	{
		if (out_hit)
			*out_hit = hit;
		if (!state_frame) // if the server is fast-forward simulating us, we can't register the hit ourselves
			hit_entity(hit);
		return true;
	}
	else
		get<Transform>()->absolute_pos(next_pos);

	return false;
}

r32 Bolt::particle_accumulator;
void Bolt::update_client_all(const Update& u)
{
	const r32 particle_interval = 0.035f;
	particle_accumulator += u.time.delta;
	while (particle_accumulator > particle_interval)
	{
		particle_accumulator -= particle_interval;
		for (auto i = list.iterator(); !i.is_last(); i.next())
		{
			if (i.item()->reflected || i.item()->type == Type::Turret)
			{
				Vec3 pos = i.item()->get<Transform>()->absolute_pos();
				Particles::tracers.add
				(
					Vec3::lerp(particle_accumulator / vi_max(0.0001f, u.time.delta), i.item()->last_pos, pos),
					Vec3::zero,
					0
				);
			}
		}
	}

	for (auto i = list.iterator(); !i.is_last(); i.next())
		i.item()->last_pos = i.item()->get<Transform>()->absolute_pos();
}

s16 Bolt::raycast_mask(AI::Team team)
{
	return ~Team::force_field_mask(team);
}

void Bolt::reflect(const Entity* hit_object, ReflectionType reflection_type, const Vec3& normal)
{
	vi_assert(Game::level.local);
	Transform* transform = get<Transform>();

	Vec3 dir;
	if (reflection_type == ReflectionType::Homing)
	{
		if (owner.ref())
			dir = Vec3::normalize(owner.ref()->get<Target>()->absolute_pos() - transform->absolute_pos());
		else
			dir = transform->absolute_rot() * Vec3(0, 0, -1.0f);
	}
	else // simple reflection
		dir = (transform->absolute_rot() * Vec3(0, 0, 1.0f)).reflect(normal);

	velocity = dir * speed(type, true);
	remaining_lifetime = (DRONE_MAX_DISTANCE * 0.99f) / (1.5f * speed(type, true));
	transform->absolute_rot(Quat::look(dir));
	transform->absolute_pos(transform->absolute_pos() + dir * BOLT_LENGTH);

	if (reflection_type == ReflectionType::Homing)
	{
		// change team
		AI::Team reflect_team = AI::entity_team(hit_object);
		BoltNet::reflect(this, reflect_team, PlayerManager::owner(hit_object), hit_object);
	}
	else
		BoltNet::reflect(this);
}

void Bolt::hit_entity(const Hit& hit, const Net::StateFrame* state_frame)
{
	Entity* hit_object = hit.entity;

	b8 destroy = true;

	b8 hit_force_field_collision = false;
	if (hit_object->has<ForceFieldCollision>())
	{
		hit_object = hit_object->get<ForceFieldCollision>()->field.ref()->entity();
		hit_force_field_collision = true;
	}

	Vec3 basis;
	if (hit_object->has<Health>())
	{
		if (reflected && hit_object->has<Minion>())
			destroy = false;
		basis = Vec3::normalize(velocity);
		s8 damage;
		switch (type)
		{
			case Type::DroneBolter:
			{
				if (hit_object->has<Battery>() || hit_object->has<Minion>())
					damage = 3;
				else if (hit_object->has<ForceField>())
					damage = 2;
				else if (hit_object->has<Drone>())
					damage = 1;
				else
					damage = 1 + (mersenne::rand() % 2); // expected value: 1.5

				if (reflected)
					damage = 12;
				break;
			}
			case Type::DroneShotgun:
			{
				if (hit_object->has<Battery>())
					damage = BATTERY_HEALTH;
				else if (hit_object->has<Minion>())
					damage = MINION_HEALTH;
				else if (hit_object->has<Drone>() || hit_object->has<ForceField>())
					damage = 1 + (mersenne::rand() % 2); // expected value: 1.5
				else
					damage = 2;

				if (reflected)
				{
					if (hit_object->has<Drone>())
						damage = 1;
					else
						damage = MINION_HEALTH;
				}
				break;
			}
			case Type::Minion:
			{
				if (hit_object->has<Drone>() || hit_object->has<Minion>())
					damage = 1;
				else if (hit_object->has<ForceField>())
					damage = 4;
				else
					damage = 2;

				if (reflected)
					damage = 12;
				break;
			}
			case Type::Turret:
			{
				if (reflected)
					damage = 12;
				else
				{
					if (hit_object->has<Minion>())
						damage = MINION_HEALTH / 2;
					else
						damage = 1;
				}
				break;
			}
			default:
			{
				damage = 0;
				vi_assert(false);
				break;
			}
		}

		if (!hit_object->has<Health>() || !hit_object->get<Health>()->can_take_damage(entity(), state_frame))
			damage = 0;

		if (hit_force_field_collision)
		{
			if (!reflected)
			{
				destroy = false;
				reflect(hit_object, ReflectionType::Simple, hit.normal);
			}
		}
		else if (hit_object->get<Health>()->active_armor(state_frame))
		{
			damage = 0;
			if (hit_object->has<Shield>())
			{
				// reflect
				destroy = false;
				reflect(hit_object);
			}
		}

		if (damage > 0)
		{
			if (hit_object->get<Health>()->damage_buffer_required(entity()))
			{
				// wait for damage buffer
				destroy = false;
				velocity = Vec3::zero;
				remaining_lifetime = NET_MAX_RTT_COMPENSATION * 2.0f;
			}
			hit_object->get<Health>()->damage(entity(), damage);
		}

		if (hit_object->has<RigidBody>())
		{
			RigidBody* body = hit_object->get<RigidBody>();
			body->btBody->applyImpulse(velocity * 0.1f, Vec3::zero);
			body->btBody->activate(true);
		}
	}
	else
		basis = hit.normal;

	{
		ParticleEffect::Type particle_type;
		if (type == Type::DroneShotgun)
			particle_type = ParticleEffect::Type::ImpactTiny;
		else if (hit_object->has<Health>())
			particle_type = ParticleEffect::Type::ImpactLarge;
		else
			particle_type = ParticleEffect::Type::ImpactSmall;
		ParticleEffect::spawn(particle_type, hit.point, Quat::look(basis));
	}

	if (destroy)
		World::remove_deferred(entity());
}

Array<ParticleEffect> ParticleEffect::list;

template<typename Stream> b8 serialize_particle_effect(Stream* p, ParticleEffect* e)
{
	serialize_enum(p, ParticleEffect::Type, e->type);
	serialize_ref(p, e->owner);
	serialize_ref(p, e->parent);
	serialize_s8(p, e->team);
	if (!Net::serialize_position(p, &e->pos, Net::Resolution::Low))
		net_error();
	if (!Net::serialize_quat(p, &e->rot, Net::Resolution::Low))
		net_error();
	if (Stream::IsReading)
	{
		e->abs_pos = e->pos;
		e->abs_rot = e->rot;
		if (Transform* parent = e->parent.ref())
			parent->to_world(&e->abs_pos, &e->abs_rot);
	}
	return true;
}

b8 ParticleEffect::spawn(Type t, const Vec3& pos, const Quat& rot, Transform* parent, PlayerManager* owner, AI::Team team)
{
	vi_assert(Game::level.local);
	using Stream = Net::StreamWrite;
	Net::StreamWrite* p = Net::msg_new(Net::MessageType::ParticleEffect);

	ParticleEffect e;
	e.type = t;
	e.pos = e.abs_pos = pos;
	e.rot = e.abs_rot = rot;
	if (parent)
		parent->to_local(&e.pos, &e.rot);
	e.parent = parent;
	e.team = team;
	e.owner = owner;
	if (!serialize_particle_effect(p, &e))
		net_error();

	Net::msg_finalize(p);

	return true;
}

b8 ParticleEffect::net_msg(Net::StreamRead* p)
{
	using Stream = Net::StreamRead;

	ParticleEffect e;
	if (!serialize_particle_effect(p, &e))
		net_error();

	if (e.type == Type::Grenade)
	{
		Audio::post_global(AK::EVENTS::PLAY_DRONE_GRENADE_EXPLO, e.abs_pos);
		EffectLight::add(e.abs_pos, GRENADE_RANGE, 0.35f, EffectLight::Type::Explosion);
	}
	else if (e.type == Type::Explosion)
	{
		Audio::post_global(AK::EVENTS::PLAY_EXPLOSION, e.abs_pos);
		EffectLight::add(e.abs_pos, 8.0f, 0.35f, EffectLight::Type::Explosion);
	}
	else if (e.type == Type::DroneExplosion)
		EffectLight::add(e.abs_pos, 8.0f, 0.35f, EffectLight::Type::Explosion);
	else if (e.type == Type::ImpactLarge || e.type == Type::ImpactSmall)
		Audio::post_global(AK::EVENTS::PLAY_DRONE_BOLT_IMPACT, e.abs_pos);
	else if (e.type == Type::SpawnMinion)
		Audio::post_global(AK::EVENTS::PLAY_MINION_SPAWN, e.abs_pos);
	else if (e.type == Type::SpawnDrone)
		Audio::post_global(AK::EVENTS::PLAY_DRONE_SPAWN, e.abs_pos);
	else if (e.type == Type::SpawnForceField || e.type == Type::SpawnBatteryForceField)
		Audio::post_global(AK::EVENTS::PLAY_FORCE_FIELD_SPAWN, e.abs_pos);
	else if (e.type == Type::SpawnRectifier)
		Audio::post_global(AK::EVENTS::PLAY_RECTIFIER_SPAWN, e.abs_pos);
	else if (e.type == Type::SpawnMinionSpawner)
		Audio::post_global(AK::EVENTS::PLAY_MINION_SPAWNER_SPAWN, e.abs_pos);
	else if (e.type == Type::SpawnTurret)
		Audio::post_global(AK::EVENTS::PLAY_TURRET_SPAWN, e.abs_pos);

	if (e.type == Type::Grenade)
	{
		// camera shake
		for (auto i = PlayerControlHuman::list.iterator(); !i.is_last(); i.next())
		{
			r32 distance = (i.item()->get<Transform>()->absolute_pos() - e.abs_pos).length();
			if (distance < GRENADE_RANGE * 1.5f)
				i.item()->camera_shake(LMath::lerpf(vi_max(0.0f, (distance - (GRENADE_RANGE * 0.66f)) / (GRENADE_RANGE * (1.5f - 0.66f))), 1.0f, 0.0f));
		}

		// grenade impulses
		for (auto i = Grenade::list.iterator(); !i.is_last(); i.next())
		{
			if (i.item()->state == Grenade::State::Inactive || i.item()->state == Grenade::State::Active)
			{
				Vec3 to_item = i.item()->get<Transform>()->absolute_pos() - e.abs_pos;
				r32 distance = to_item.length();
				to_item /= distance;
				if (distance < GRENADE_RANGE)
				{
					i.item()->velocity += to_item * LMath::lerpf(distance / GRENADE_RANGE, 1.0f, 0.0f) * GRENADE_LAUNCH_SPEED * 2.0f;
					r32 length = i.item()->velocity.length();
					if (length > GRENADE_LAUNCH_SPEED * 1.5f)
						i.item()->velocity *= (GRENADE_LAUNCH_SPEED * 1.5f) / length; // prevent insane velocity
				}
			}
		}

		// physics impulses
		for (auto i = RigidBody::list.iterator(); !i.is_last(); i.next())
		{
			if (i.item()->mass)
			{
				Vec3 to_item = i.item()->get<Transform>()->absolute_pos() - e.abs_pos;
				r32 distance = to_item.length();
				to_item /= distance;
				if (distance < GRENADE_RANGE)
				{
					RigidBody* body = i.item()->get<RigidBody>();
					body->btBody->applyImpulse(to_item * LMath::lerpf(distance / GRENADE_RANGE, 1.0f, 0.0f) * 10.0f, Vec3::zero);
					body->btBody->activate(true);
				}
			}
		}
	}

	if (e.type == Type::ImpactLarge)
		EffectLight::add(e.abs_pos, GRENADE_RANGE * 0.5f, 0.75f, EffectLight::Type::Shockwave);

	if (is_spawn_effect(e.type))
	{
		e.lifetime = SPAWN_EFFECT_LIFETIME;
		list.add(e);
	}
	else
	{
		for (s32 i = 0; i < 50; i++)
		{
			Particles::sparks.add
			(
				e.abs_pos,
				e.abs_rot * Vec3(mersenne::randf_oo() * 2.0f - 1.0f, mersenne::randf_oo() * 2.0f - 1.0f, mersenne::randf_oo()) * 10.0f,
				Vec4(1, 1, 1, 1)
			);
		}
	}

	return true;
}

b8 ParticleEffect::is_spawn_effect(Type t)
{
	return t == Type::SpawnDrone
		|| t == Type::SpawnMinion
		|| t == Type::SpawnForceField
		|| t == Type::SpawnBatteryForceField
		|| t == Type::SpawnRectifier
		|| t == Type::SpawnMinionSpawner
		|| t == Type::SpawnTurret;
}

#define SPAWN_EFFECT_THRESHOLD 0.4f

void ParticleEffect::update_all(const Update& u)
{
	for (s32 i = 0; i < list.length; i++)
	{
		ParticleEffect* e = &list[i];
		e->lifetime -= u.time.delta;
		if (e->lifetime < 0.0f)
		{
			list.remove(i);
			i--;
		}
		else
		{
			e->abs_pos = e->pos;
			e->abs_rot = e->rot;
			if (Transform* parent = e->parent.ref())
				parent->to_world(&e->abs_pos, &e->abs_rot);

			const r32 threshold = SPAWN_EFFECT_LIFETIME - SPAWN_EFFECT_THRESHOLD;
			if (e->lifetime < threshold && e->lifetime + u.time.delta >= threshold)
			{
				EffectLight::add(e->abs_pos, GRENADE_RANGE * 0.5f, 0.75f, EffectLight::Type::Shockwave);
				if (Game::level.local && (e->owner.ref() || e->type == Type::SpawnMinion || e->type == Type::SpawnForceField || e->type == Type::SpawnBatteryForceField))
				{
					switch (e->type)
					{
						case Type::SpawnDrone:
							break;
						case Type::SpawnMinion:
						{
							AI::Team team = e->owner.ref() ? e->owner.ref()->team.ref()->team() : e->team;
							Net::finalize(World::create<MinionEntity>(e->abs_pos + Vec3(0, 1, 0), e->abs_rot, team, e->owner.ref()));
							break;
						}
						case Type::SpawnForceField:
						{
							AI::Team team = e->owner.ref() ? e->owner.ref()->team.ref()->team() : e->team;
							Net::finalize(World::create<ForceFieldEntity>(e->parent.ref(), e->abs_pos, e->abs_rot, team));
							break;
						}
						case Type::SpawnBatteryForceField:
						{
							AI::Team team = e->owner.ref() ? e->owner.ref()->team.ref()->team() : e->team;
							Net::finalize(World::create<ForceFieldEntity>(e->parent.ref(), e->abs_pos, e->abs_rot, team, ForceField::Type::Battery));
							break;
						}
						case Type::SpawnRectifier:
							Net::finalize(World::create<RectifierEntity>(e->owner.ref(), e->abs_pos, e->abs_rot, e->parent.ref()));
							break;
						case Type::SpawnMinionSpawner:
						{
							AI::Team team = e->owner.ref() ? e->owner.ref()->team.ref()->team() : e->team;
							Net::finalize(World::create<MinionSpawnerEntity>(e->owner.ref(), team, e->abs_pos, e->abs_rot, e->parent.ref()));
							break;
						}
						case Type::SpawnTurret:
						{
							AI::Team team = e->owner.ref() ? e->owner.ref()->team.ref()->team() : e->team;
							Net::finalize(World::create<TurretEntity>(e->owner.ref(), team, e->abs_pos, e->abs_rot, e->parent.ref()));
							break;
						}
						default:
							vi_assert(false);
							break;
					}
				}
			}
		}
	}
}

void ParticleEffect::draw_alpha(const RenderParams& p)
{
	for (s32 i = 0; i < list.length; i++)
	{
		const ParticleEffect& e = list[i];

		Mat4 m;
		Vec3 size;
		AssetID mesh;

		{
			r32 blend = 1.0f - (e.lifetime / SPAWN_EFFECT_LIFETIME);
			r32 radius_scale;
			r32 height_scale;
			const r32 fulcrum = SPAWN_EFFECT_THRESHOLD / SPAWN_EFFECT_LIFETIME;
			if (blend < fulcrum)
			{
				blend /= fulcrum; // blend goes from 0 to 1
				radius_scale = 1.0f;
				height_scale = Ease::cubic_in_out<r32>(blend);
			}
			else
			{
				blend = ((blend - fulcrum) / (1.0f - fulcrum)); // blend goes from 0 to 1
				radius_scale = LMath::lerpf(Ease::cubic_in_out<r32>(vi_min(blend / 0.5f, 1.0f)), 1.0f, 0.05f);
				height_scale = 1.0f - Ease::cubic_in<r32>(blend);
			}

			switch (e.type)
			{
				case Type::SpawnDrone:
				case Type::SpawnRectifier:
				case Type::SpawnTurret:
				{
					size = Vec3(height_scale * DRONE_SHIELD_RADIUS * 1.1f);
					m.make_transform(e.abs_pos, size, Quat::identity);
					mesh = Asset::Mesh::sphere_highres;
					break;
				}
				case Type::SpawnMinionSpawner:
				{
					size = Vec3(radius_scale * 1.01f, height_scale * 0.6f, radius_scale * 1.1f);
					m.make_transform(e.abs_pos + e.abs_rot * Vec3(0, 0, -MINION_SPAWNER_RADIUS), size, e.abs_rot * Quat::euler(0, 0, PI * 0.5f));
					mesh = Asset::Mesh::cylinder;
					break;
				}
				case Type::SpawnMinion:
				{
					size = Vec3(radius_scale * WALKER_MINION_RADIUS * 1.5f, height_scale * (WALKER_HEIGHT * 2.0f + WALKER_SUPPORT_HEIGHT), radius_scale * WALKER_MINION_RADIUS * 1.5f);
					m.make_transform(e.abs_pos, size, Quat::identity);
					mesh = Asset::Mesh::cylinder;
					break;
				}
				case Type::SpawnForceField:
				case Type::SpawnBatteryForceField:
				{
					size = Vec3(radius_scale * FORCE_FIELD_BASE_RADIUS * 1.75f, height_scale * (FORCE_FIELD_BASE_OFFSET + DRONE_RADIUS * 2.0f), radius_scale * FORCE_FIELD_BASE_RADIUS * 1.75f);
					m.make_transform(e.abs_pos + e.abs_rot * Vec3(0, 0, -FORCE_FIELD_BASE_OFFSET), size, e.abs_rot * Quat::euler(0, 0, PI * 0.5f));
					mesh = Asset::Mesh::cylinder;
					break;
				}
				default:
					vi_assert(false);
					break;
			}
		}

		r32 radius = vi_max(size.x, vi_max(size.y, size.z));
		View::draw_mesh(p, mesh, Asset::Shader::standard_flat, AssetNull, m, Vec4(1), radius);
	}
}

void ParticleEffect::clear()
{
	list.length = 0;
}

Array<ShellCasing> ShellCasing::list;

#define SHELL_CASING_LIFETIME 2.5f

Vec3 shell_casing_size(ShellCasing::Type type)
{
	switch (type)
	{
		case ShellCasing::Type::Bolter:
			return Vec3(0.04f, 0.04f, 0.06f);
		case ShellCasing::Type::Shotgun:
			return Vec3(0.06f, 0.06f, 0.08f);
		case ShellCasing::Type::Sniper:
			return Vec3(0.04f, 0.04f, 0.12f);
		default:
		{
			vi_assert(false);
			return Vec3::zero;
			break;
		}
	}
}

void ShellCasing::spawn(const Vec3& pos, const Quat& rot, Type type)
{
	if (!Settings::shell_casings)
		return;

	ShellCasing* entry = list.add();
	entry->type = type;
	entry->pos = pos;
	entry->rot = rot;
	entry->timer = SHELL_CASING_LIFETIME;
	Vec3 size = shell_casing_size(type);
	entry->btShape = new btBoxShape(size);
	btVector3 localInertia(0, 0, 0);
	const r32 mass = 0.01f;
	if (mass > 0.0f)
		entry->btShape->calculateLocalInertia(mass, localInertia);

	btRigidBody::btRigidBodyConstructionInfo info(mass, 0, entry->btShape, localInertia);

	info.m_startWorldTransform = btTransform(entry->rot, entry->pos);
	entry->btBody = new btRigidBody(info);
	entry->btBody->setWorldTransform(btTransform(entry->rot, entry->pos));

	entry->btBody->setRestitution(1.0f);
	entry->btBody->setUserIndex(-1);
	entry->btBody->setCcdMotionThreshold(size.x);
	entry->btBody->setCcdSweptSphereRadius(size.z);

	entry->btBody->setLinearVelocity(rot * Vec3(-0.707f * 4.0f, 0.707f * 4.0f + mersenne::randf_cc() - 0.5f, 0));
	entry->btBody->setAngularVelocity(btVector3((mersenne::randf_cc() - 0.5f) * 2.0f, (mersenne::randf_cc() - 0.5f) * 2.0f, (mersenne::randf_cc() - 0.5f) * 2.0f));

	Physics::btWorld->addRigidBody(entry->btBody, CollisionDroneIgnore, CollisionStatic);
}

void ShellCasing::sync_physics()
{
	for (s32 i = 0; i < list.length; i++)
	{
		ShellCasing* s = &list[i];
		btTransform transform = s->btBody->getInterpolationWorldTransform();
		s->pos = transform.getOrigin();
		s->rot = transform.getRotation();
	}
}

void ShellCasing::update_all(const Update& u)
{
	// rigid body transforms are synced in Physics::sync_dynamic()

	if (list.length > 0)
	{
		if (Settings::shell_casings)
		{
			for (s32 i = 0; i < list.length; i++)
			{
				ShellCasing* s = &list[i];
				s->timer -= u.time.delta;
				if (s->timer < 0.0f)
				{
					s->cleanup();
					list.remove(i);
					i--;
				}
			}
		}
		else
			clear();
	}
}

void ShellCasing::clear()
{
	for (s32 i = 0; i < list.length; i++)
		list[i].cleanup();
	list.length = 0;
}

void ShellCasing::cleanup()
{
	Physics::btWorld->removeRigidBody(btBody);
	delete btBody;
	delete btShape;
}

Array<InstanceVertex> ShellCasing::instances;
void ShellCasing::draw_all(const RenderParams& params)
{
	if (!params.camera->mask || !Settings::shell_casings)
		return;

	const Mesh* mesh_data = Loader::mesh_instanced(Asset::Mesh::shell_casing);
	Vec3 radius = (Vec4(mesh_data->bounds_radius, mesh_data->bounds_radius, mesh_data->bounds_radius, 0)).xyz();
	r32 f_radius = vi_max(radius.x, vi_max(radius.y, radius.z));

	RenderSync* sync = params.sync;

	if (params.update_instances())
	{
		instances.length = 0;
		for (s32 i = 0; i < list.length; i++)
		{
			const ShellCasing& s = list[i];
			Mat4 m;
			m.make_transform(s.pos, Vec3(1), s.rot);

			Vec3 size = shell_casing_size(s.type);
			if (params.camera->visible_sphere(m.translation(), size.z * f_radius))
			{
				m.scale(size);
				instances.add({ m, Vec4(1) });
			}
		}

		if (instances.length > 0)
		{
			sync->write(RenderOp::UpdateInstances);
			sync->write(Asset::Mesh::shell_casing);
			sync->write(instances.length);
			sync->write<InstanceVertex>(instances.data, instances.length);
		}
	}

	if (instances.length == 0)
		return;

	Loader::shader_permanent(Asset::Shader::standard_instanced);

	sync->write(RenderOp::Shader);
	sync->write(Asset::Shader::standard_instanced);
	sync->write(params.technique);

	Mat4 vp = params.view_projection;

	sync->write(RenderOp::Uniform);
	sync->write(Asset::Uniform::vp);
	sync->write(RenderDataType::Mat4);
	sync->write<s32>(1);
	sync->write<Mat4>(vp);

	sync->write(RenderOp::Uniform);
	sync->write(Asset::Uniform::v);
	sync->write(RenderDataType::Mat4);
	sync->write<s32>(1);
	sync->write<Mat4>(params.view);

	sync->write(RenderOp::Uniform);
	sync->write(Asset::Uniform::diffuse_color);
	sync->write(RenderDataType::Vec4);
	sync->write<s32>(1);
	sync->write<Vec4>(Vec4(1, 1, 1, MATERIAL_NO_OVERRIDE));

	sync->write(params.flags & RenderFlagEdges ? RenderOp::InstancesEdges : RenderOp::Instances);
	sync->write(Asset::Mesh::shell_casing);
}

GrenadeEntity::GrenadeEntity(PlayerManager* owner, const Vec3& abs_pos, const Vec3& dir)
{
	Transform* transform = create<Transform>();
	transform->absolute_pos(abs_pos);
	transform->absolute_rot(Quat::look(dir));

	create<Audio>();

	Grenade* g = create<Grenade>();
	g->team = owner->team.ref()->team();
	g->owner = owner;
	g->velocity = dir * GRENADE_LAUNCH_SPEED;

	create<RigidBody>(RigidBody::Type::Sphere, Vec3(GRENADE_RADIUS * 2.0f), 0.0f, CollisionTarget, ~CollisionParkour & ~CollisionElectric & ~CollisionStatic & ~CollisionAudio & ~CollisionInaccessible & ~CollisionAllTeamsForceField & ~CollisionWalker & ~CollisionMinionMoving & ~CollisionGlass);

	PointLight* light = create<PointLight>();
	light->radius = BOLT_LIGHT_RADIUS;
	light->color = Team::color_neutral().xyz();

	View* model = create<View>();
	model->color = Team::color_neutral();
	model->mesh = Asset::Mesh::grenade_detached;
	model->team = s8(owner->team.ref()->team());
	model->shader = Asset::Shader::standard;
	model->offset.scale(Vec3(GRENADE_RADIUS));

	create<Health>(GRENADE_HEALTH, GRENADE_HEALTH);

	create<Target>();
}

b8 Grenade::net_msg(Net::StreamRead* p, Net::MessageSource src)
{
	using Stream = Net::StreamRead;
	Ref<Grenade> ref;
	serialize_ref(p, ref);

	State state;
	serialize_enum(p, State, state);

	Ref<Transform> parent;
	if (state == State::Attached)
		serialize_ref(p, parent);

	if (Game::level.local == (src == Net::MessageSource::Loopback))
	{
		if (state == State::Attached)
		{
			if (parent.ref()->has<Minion>())
				parent.ref()->get<Minion>()->carrying = ref.ref()->entity();
			ref.ref()->get<Audio>()->post(AK::EVENTS::PLAY_GRENADE_ATTACH);
		}
		else if (ref.ref()->state == State::Inactive && state == State::Active)
			ref.ref()->get<Audio>()->post(AK::EVENTS::PLAY_GRENADE_ARM);

		ref.ref()->state = state;
		View* view = ref.ref()->get<View>();
		view->mesh = (state == State::Attached) ? Asset::Mesh::grenade_attached : Asset::Mesh::grenade_detached;
		view->mask = (state == State::Exploded) ? 0 : RENDER_MASK_DEFAULT;
	}

	return true;
}

namespace GrenadeNet
{
	b8 send_state_change(Grenade* g, Grenade::State s)
	{
		using Stream = Net::StreamWrite;
		Net::StreamWrite* p = Net::msg_new(Net::MessageType::Grenade);
		{
			Ref<Grenade> ref = g;
			serialize_ref(p, ref);
		}
		serialize_enum(p, Grenade::State, s);
		if (s == Grenade::State::Attached)
			serialize_ref(p, g->get<Transform>()->parent);
		Net::msg_finalize(p);
		return true;
	}
}

b8 grenade_trigger_filter(Entity* e, AI::Team team)
{
	return (e->has<Minion>() && e->get<AIAgent>()->team != team)
		|| (e->has<ForceField>() && e->get<ForceField>()->team != team)
		|| (e->has<ForceFieldCollision>() && e->get<ForceFieldCollision>()->field.ref()->team != team)
		|| (e->has<Rectifier>() && !e->has<Battery>() && e->get<Rectifier>()->team != team)
		|| (e->has<Turret>() && e->get<Turret>()->team != team)
		|| (e->has<MinionSpawner>() && e->get<MinionSpawner>()->team != team)
		|| (e->has<Drone>() && e->get<AIAgent>()->team != team && !UpgradeStation::drone_inside(e->get<Drone>()));
}

b8 grenade_hit_filter(Entity* e, AI::Team team)
{
	return e->has<Minion>() || grenade_trigger_filter(e, team);
}

// returns true if grenade hits something
b8 Grenade::simulate(r32 dt, Bolt::Hit* out_hit, const Net::StateFrame* state_frame)
{
	vi_assert(Game::level.local);
	Transform* t = get<Transform>();
	if (!t->parent.ref() && state != State::Exploded)
	{
		Vec3 pos = t->absolute_pos();
		Vec3 next_pos;
		{
			Vec3 half_accel = Physics::btWorld->getGravity() * dt * 0.5f;
			velocity += half_accel;
			next_pos = pos + velocity * dt;
			velocity += half_accel;
		}

		if (next_pos.y < Game::level.min_y)
		{
			World::remove_deferred(entity());
			return false;
		}

		if (!btVector3(next_pos - pos).fuzzyZero())
		{
			Glass::shatter_all(pos, next_pos);

			Bolt::Hit hit;
			if (Bolt::raycast(pos, next_pos, CollisionStatic | (CollisionAllTeamsForceField & Bolt::raycast_mask(team)), team, &hit, &grenade_hit_filter, state_frame, DRONE_SHIELD_RADIUS * DRONE_SHIELD_VIEW_RATIO))
			{
				if (out_hit)
					*out_hit = hit;

				if (!state_frame) // if server is fast-forward simulating us, we can't register the hit ourselves
					hit_entity(hit);

				return true;
			}
		}

		t->pos = next_pos;
	}

	return false;
}

void Grenade::hit_entity(const Bolt::Hit& hit, const Net::StateFrame* state_frame)
{
	if (grenade_hit_filter(hit.entity, team))
	{
		if (hit.entity->has<Minion>())
		{
			if (hit.entity->get<Minion>()->carrying.ref())
			{
				// bounce off minion

				if (hit.entity->get<AIAgent>()->team != team)
					timer = vi_max(timer, GRENADE_DELAY - GRENADE_QUICK_FUSE); // quick fuse

				velocity = velocity.reflect(hit.normal) * 0.5f;
				if (state != State::Active)
					GrenadeNet::send_state_change(this, State::Active);
			}
			else
			{
				// stick to minion
				velocity = Vec3::zero;
				get<Transform>()->parent = hit.entity->get<Transform>();
				get<Transform>()->pos = Vec3::zero;
				get<Transform>()->rot = Quat::identity;

				GrenadeNet::send_state_change(this, State::Attached);
			}
		}
		else
		{
			// quick fuse
			timer = vi_max(timer, GRENADE_DELAY - 0.3f);

			// bounce
			velocity = velocity.reflect(hit.normal) * 0.5f;
			if (state != State::Active)
				GrenadeNet::send_state_change(this, State::Active);
		}
	}
	else
	{
		if (state == State::Active)
		{
			// attach
			velocity = Vec3::zero;
			get<Transform>()->parent = hit.entity->get<Transform>();
			get<Transform>()->absolute(hit.point + hit.normal * GRENADE_RADIUS * 1.1f, Quat::look(hit.normal));
			GrenadeNet::send_state_change(this, State::Attached);
		}
		else
		{
			// bounce
			velocity = velocity.reflect(hit.normal) * 0.5f;
			GrenadeNet::send_state_change(this, State::Active);
		}
	}
}

b8 grenade_line_of_sight(Entity* grenade, AI::Team my_team, const Vec3& me, const Vec3& pos, const Entity* target)
{
	if (grenade == target)
		return false;

	if (btVector3(me - pos).fuzzyZero())
		return true;

	b8 line_of_sight = true;
	btCollisionWorld::ClosestRayResultCallback ray_callback(me, pos);
	Physics::raycast(&ray_callback, CollisionStatic | (CollisionAllTeamsForceField & Bolt::raycast_mask(my_team)));
	if (ray_callback.hasHit())
	{
		// something's blocking the way
		// but we might be trying to damage a force field, so see if the thing blocking our way is the actual force field
		Entity* entity = &Entity::list[ray_callback.m_collisionObject->getUserIndex()];
		if (entity->has<ForceFieldCollision>())
			entity = entity->get<ForceFieldCollision>()->field.ref()->entity();
		line_of_sight = entity == target;
	}
	return line_of_sight;
}

void Grenade::explode()
{
	vi_assert(Game::level.local);
	vi_assert(state != State::Exploded);

	GrenadeNet::send_state_change(this, State::Exploded);

	timer = 0.0f; // stick around for a bit after exploding to make sure damage gets counted properly

	Vec3 me = get<Transform>()->absolute_pos();
	ParticleEffect::spawn(ParticleEffect::Type::Grenade, me, Quat::look(Vec3(0, 1, 0))); // also handles physics impulses

	for (auto i = Glass::list.iterator(); !i.is_last(); i.next())
	{
		Vec3 pos = i.item()->get<Transform>()->absolute_pos();
		Vec3 diff = pos - me;
		if (diff.length() < (GRENADE_RANGE * 0.75f) + i.item()->get<View>()->radius)
			i.item()->shatter(pos, diff);
	}

	for (auto i = Health::list.iterator(); !i.is_last(); i.next())
	{
		if (i.item()->can_take_damage(entity()))
		{
			Vec3 pos = i.item()->get<Transform>()->absolute_pos();
			r32 distance = (pos - me).length();

			if (i.item()->has<ForceField>())
				distance -= FORCE_FIELD_RADIUS;

			s8 damage = 0;
			if (i.item()->has<Drone>())
			{
				distance *= (i.item()->get<AIAgent>()->team == team) ? 2.0f : 1.0f;
				if (distance < GRENADE_RANGE * 0.4f)
					damage = 3;
				else if (distance < GRENADE_RANGE * 0.7f)
					damage = 2;
				else if (distance < GRENADE_RANGE)
					damage = 1;
			}
			else if (i.item()->has<MinionSpawner>())
			{
				distance *= (i.item()->get<MinionSpawner>()->team == team) ? 2.0f : 1.0f;
				if (distance < GRENADE_RANGE)
					damage = MINION_SPAWNER_HEALTH + DRONE_SHIELD_AMOUNT;
			}
			else if (i.item()->has<Turret>())
			{
				distance *= (i.item()->get<Turret>()->team == team) ? 2.0f : 1.0f;
				if (distance < GRENADE_RANGE * 0.5f)
					damage = TURRET_HEALTH + DRONE_SHIELD_AMOUNT;
				else if (distance < GRENADE_RANGE)
					damage = TURRET_HEALTH + DRONE_SHIELD_AMOUNT - 6;
			}
			else if (i.item()->has<ForceField>())
			{
				if (i.item()->get<ForceField>()->team != team)
				{
					if (distance < GRENADE_RANGE)
						damage = FORCE_FIELD_HEALTH_NORMAL - 20;
				}
			}
			else if (i.item()->has<Grenade>() && i.item()->get<Grenade>()->team == team)
			{
				// don't damage friendly grenades (no chain reaction)
			}
			else if (i.item()->has<Minion>())
			{
				if (distance < GRENADE_RANGE)
					damage = MINION_HEALTH;
			}
			else
			{
				if ((i.item()->has<Rectifier>() && i.item()->get<Rectifier>()->team == team)
					|| (i.item()->has<Battery>() && i.item()->get<Battery>()->team == team))
					distance *= 2.0f;

				if (distance < GRENADE_RANGE)
					damage = distance < GRENADE_RANGE * 0.5f ? 6 : (distance < GRENADE_RANGE * 0.75f ? 3 : 1);
			}

			if (damage > 0 && grenade_line_of_sight(entity(), team, me, pos, i.item()->entity()))
				i.item()->damage(entity(), damage);
		}
	}
}

void Grenade::set_owner(PlayerManager* m)
{
	// not synced over network
	owner = m;
}

r32 Grenade::particle_accumulator;
void Grenade::update_all(const Update& u)
{
	// normal particles
	const r32 interval = 0.05f;
	particle_accumulator += u.time.delta;
	b8 emit_particles = false;
	while (particle_accumulator > interval)
	{
		particle_accumulator -= interval;
		emit_particles = true;
	}

	for (auto i = list.iterator(); !i.is_last(); i.next())
	{
		if (i.item()->state == State::Exploded)
		{
			if (Game::level.local)
			{
				i.item()->timer += u.time.delta;
				if (i.item()->timer > NET_MAX_RTT_COMPENSATION * 2.0f)
					World::remove(i.item()->entity());
			}
		}
		else
		{
			Transform* transform = i.item()->get<Transform>();

			// if we're stuck to a minion, make it look like we're attached to the minion's back
			if (i.item()->state == State::Attached)
			{
				View* model = i.item()->get<View>();
				Transform* parent = transform->parent.ref();
				if (parent)
				{
					if (parent->has<Animator>())
					{
						Vec3 pos(0.1f, 0.0f, -0.12f - GRENADE_RADIUS);
						Quat rot = Quat::euler(0, PI, 0);
						parent->get<Animator>()->to_local(Asset::Bone::character_spine, &pos, &rot);
						model->offset.make_transform(pos, Vec3(GRENADE_RADIUS), rot);
						i.item()->get<Target>()->local_offset = pos;
						i.item()->abs_pos_attached = parent->to_world(pos);
					}
					else
						i.item()->abs_pos_attached = transform->absolute_pos();
				}
				else
				{
					// no longer attached
					model->offset = Mat4::make_scale(Vec3(GRENADE_RADIUS));
					i.item()->get<Target>()->local_offset = Vec3::zero;
					if (Game::level.local)
					{
						transform->pos = i.item()->abs_pos_attached;
						transform->rot = Quat::identity;
						GrenadeNet::send_state_change(i.item(), State::Active);
					}
				}
			}
			else // Active or Inactive
			{
				if (emit_particles)
					Particles::tracers.add(i.item()->get<Transform>()->absolute_pos(), Vec3::zero, 0);
				if (Game::level.local)
					i.item()->simulate(u.time.delta);
			}

			Vec3 me = transform->absolute_pos();

			b8 countdown = false;
			if (i.item()->timer > GRENADE_DELAY * 0.5f) // once the countdown is past 50%, it's irreversible
				countdown = true;
			else
			{
				for (auto j = Health::list.iterator(); !j.is_last(); j.next())
				{
					if (grenade_trigger_filter(j.item()->entity(), i.item()->team))
					{
						Vec3 pos = j.item()->get<Transform>()->absolute_pos();
						r32 distance = (pos - me).length();
						if (j.item()->has<ForceField>())
							distance -= FORCE_FIELD_RADIUS;
						if (distance < GRENADE_RANGE * 0.5f && grenade_line_of_sight(i.item()->entity(), i.item()->team, me, pos, j.item()->entity()))
						{
							countdown = true;
							break;
						}
					}
				}
			}

			if (countdown)
			{
				r32 timer_last = i.item()->timer;
				i.item()->timer += u.time.delta;
				if (i.item()->timer < GRENADE_DELAY)
				{
					const r32 interval = 1.5f;
					if (timer_last == 0.0f || s32(Ease::cubic_in<r32>(i.item()->timer) / interval) != s32(Ease::cubic_in<r32>(timer_last) / interval))
						i.item()->get<Audio>()->post(AK::EVENTS::PLAY_GRENADE_BEEP);
				}
				else
				{
					if (Game::level.local)
						i.item()->explode();
					else
						i.item()->timer = 0.0f; // the client may determine it needs to explode while the server decides otherwise; reset client timer in this case
				}
			}
			else
			{
				i.item()->timer = 0.0f;
				const r32 interval = 5.0f;
				if (s32(Game::time.total / interval) != s32((Game::time.total - u.time.delta) / interval))
					i.item()->get<Audio>()->post(AK::EVENTS::PLAY_GRENADE_BEEP);
			}
		}
	}
}

void Grenade::awake()
{
	link_arg<Entity*, &Grenade::killed_by>(get<Health>()->killed);
	link_arg<const TargetEvent&, &Grenade::hit_by>(get<Target>()->target_hit);
}

void Grenade::hit_by(const TargetEvent& e)
{
	if (state != State::Exploded)
		get<Health>()->kill(e.hit_by);
}

void Grenade::killed_by(Entity* e)
{
	if (state != State::Exploded)
	{
		PlayerManager::entity_killed_by(entity(), e);
		if (Game::level.local)
			explode();
	}
}

Vec3 Target::velocity() const
{
	if (has<Drone>())
		return get<Drone>()->velocity;
	else if (Game::level.local)
	{
		if (has<Parkour>() && !get<PlayerControlHuman>()->local())
			return net_velocity;
		else if (has<RigidBody>())
			return get<RigidBody>()->btBody->getInterpolationLinearVelocity();
		else
			return Vec3::zero;
	}
	else
		return net_velocity;
}

b8 Target::predict_intersection(const Vec3& from, r32 speed, const Net::StateFrame* state_frame, Vec3* intersection) const
{
	Vec3 pos;
	Vec3 v;
	if (state_frame)
	{
		Quat rot;
		Vec3 l; // local_offset
		Net::transform_absolute(*state_frame, get<Transform>()->id(), &pos, &rot, &l);
		pos += rot * l;

		Net::StateFrame state_frame_last;
		Net::state_frame_by_timestamp(&state_frame_last, state_frame->timestamp - Net::tick_rate());
		Vec3 pos_last;
		Quat rot_last;
		Vec3 l_last;
		Net::transform_absolute(state_frame_last, get<Transform>()->id(), &pos_last, &rot_last, &l_last);
		pos_last += rot_last * l_last;

		v = (pos - pos_last) / Net::tick_rate();
	}
	else
	{
		v = velocity();
		pos = absolute_pos();
	}

	Vec3 to_target = pos - from;
	r32 intersect_time_squared = to_target.dot(to_target) / ((speed * speed) - 2.0f * to_target.dot(v) - v.dot(v));
	if (intersect_time_squared > 0.0f)
	{
		*intersection = pos + v * sqrtf(intersect_time_squared);
		return true;
	}
	else
		return false;
}

r32 Target::radius() const
{
	if (has<Walker>())
		return MINION_HEAD_RADIUS;
	else if (has<Turret>())
		return TURRET_RADIUS;
	else if (has<Shield>())
		return DRONE_SHIELD_RADIUS;
	else if (has<Flag>())
		return 0.0f;
	else
		return get<RigidBody>()->size.y;
}

void Target::hit(Entity* hit_by)
{
	TargetEvent e;
	e.hit_by = hit_by;
	e.target = entity();
	target_hit.fire(e);
}

Vec3 Target::absolute_pos() const
{
	return get<Transform>()->to_world(local_offset);
}

PlayerTrigger::PlayerTrigger()
	: entered(), exited(), triggered(), radius(1.0f)
{

}

b8 PlayerTrigger::is_triggered(const Entity* e) const
{
	for (s32 i = 0; i < max_trigger; i++)
	{
		if (e == triggered[i].ref())
			return true;
	}
	return false;
}

b8 PlayerTrigger::is_triggered() const
{
	for (s32 i = 0; i < max_trigger; i++)
	{
		if (triggered[i].ref())
			return true;
	}
	return false;
}

void PlayerTrigger::update(const Update& u)
{
	Vec3 pos = get<Transform>()->absolute_pos();
	r32 radius_squared = radius * radius;
	for (s32 i = 0; i < max_trigger; i++)
	{
		Entity* e = triggered[i].ref();
		if (e && (e->get<Transform>()->absolute_pos() - pos).length_squared() > radius_squared)
		{
			triggered[i] = nullptr;
			exited.fire(e);
		}
	}

	for (auto i = PlayerCommon::list.iterator(); !i.is_last(); i.next())
	{
		Entity* e = i.item()->entity();
		if ((e->get<Transform>()->absolute_pos() - pos).length_squared() < radius_squared)
		{
			b8 already_triggered = false;
			s32 free_slot = -1;
			for (s32 i = 0; i < max_trigger; i++)
			{
				if (free_slot == -1 && !triggered[i].ref())
					free_slot = i;

				if (triggered[i].ref() == e)
				{
					already_triggered = true;
					break;
				}
			}

			if (!already_triggered && free_slot != -1)
			{
				triggered[free_slot] = e;
				entered.fire(e);
			}
		}
	}
}

s32 PlayerTrigger::count() const
{
	s32 count = 0;
	for (s32 i = 0; i < max_trigger; i++)
	{
		if (triggered[i].ref())
			count++;
	}
	return count;
}

Array<InstanceVertex> Rope::instances;

// draw rope segments and bolts
void Rope::draw_all(const RenderParams& params)
{
	if (!params.camera->mask)
		return;

	const Mesh* mesh_data = Loader::mesh_instanced(Asset::Mesh::tri_tube);
	Vec3 radius = (Vec4(mesh_data->bounds_radius, mesh_data->bounds_radius, mesh_data->bounds_radius, 0)).xyz();
	r32 f_radius = vi_max(radius.x, vi_max(radius.y, radius.z));

	RenderSync* sync = params.sync;

	if (params.update_instances())
	{
		instances.length = 0;
		// ropes
		{
			const Vec3 scale = Vec3(ROPE_RADIUS, ROPE_RADIUS, ROPE_SEGMENT_LENGTH * 0.5f);

			for (auto i = list.iterator(); !i.is_last(); i.next())
			{
				Mat4 m;
				i.item()->get<Transform>()->mat(&m);

				if (params.camera->visible_sphere(m.translation(), ROPE_SEGMENT_LENGTH * f_radius))
				{
					m.scale(scale);
					instances.add({ m, Vec4(1) });
				}
			}
		}

		// bolts
		const Vec3 scale = Vec3(BOLT_THICKNESS, BOLT_THICKNESS, BOLT_LENGTH * 0.5f);
		const Mat4 offset = Mat4::make_translation(0, 0, BOLT_LENGTH * 0.5f);
		for (auto i = Bolt::list.iterator(); !i.is_last(); i.next())
		{
			Mat4 m;
			i.item()->get<Transform>()->mat(&m);
			m = offset * m;
			if (i.item()->visible() // if the bolt is waiting for damage buffering, don't draw it
				&& params.camera->visible_sphere(m.translation(), BOLT_LENGTH * f_radius))
			{
				m.scale(scale);
				instances.add({ m, Vec4(1) });
			}
		}

		// fake bolts
		for (auto i = EffectLight::list.iterator(); !i.is_last(); i.next())
		{
			if (i.item()->type == EffectLight::Type::BoltDroneBolter
				|| i.item()->type == EffectLight::Type::BoltDroneShotgun)
			{
				Mat4 m;
				m.make_transform(i.item()->pos, scale, i.item()->rot);
				m = offset * m;
				if (params.camera->visible_sphere(m.translation(), BOLT_LENGTH * f_radius))
					instances.add({ m, Vec4(1) });
			}
		}

		if (instances.length > 0)
		{
			sync->write(RenderOp::UpdateInstances);
			sync->write(Asset::Mesh::tri_tube);
			sync->write(instances.length);
			sync->write<InstanceVertex>(instances.data, instances.length);
		}
	}

	if (instances.length == 0)
		return;

	Loader::shader_permanent(Asset::Shader::flat_instanced);

	sync->write(RenderOp::Shader);
	sync->write(Asset::Shader::flat_instanced);
	sync->write(params.technique);

	Mat4 vp = params.view_projection;

	sync->write(RenderOp::Uniform);
	sync->write(Asset::Uniform::vp);
	sync->write(RenderDataType::Mat4);
	sync->write<s32>(1);
	sync->write<Mat4>(vp);

	sync->write(RenderOp::Uniform);
	sync->write(Asset::Uniform::v);
	sync->write(RenderDataType::Mat4);
	sync->write<s32>(1);
	sync->write<Mat4>(params.view);

	sync->write(RenderOp::Uniform);
	sync->write(Asset::Uniform::diffuse_color);
	sync->write(RenderDataType::Vec4);
	sync->write<s32>(1);
	sync->write<Vec4>(Vec4(1, 1, 1, 1));

	sync->write(RenderOp::Instances);
	sync->write(Asset::Mesh::tri_tube);
}

RigidBody* rope_add(RigidBody* start, const Vec3& start_relative_pos, const Vec3& pos, const Quat& rot, r32 slack, RigidBody::Constraint::Type constraint_type, s8 flags)
{
	RigidBody* last_segment = start;
	Vec3 last_segment_relative_pos = start_relative_pos;
	Vec3 forward = rot * Vec3(0, 0, 1);
	while (true)
	{
		if (last_segment)
		{
			Vec3 last_segment_pos = last_segment->get<Transform>()->to_world(last_segment_relative_pos);
			Vec3 diff = pos - last_segment_pos;
			r32 length = diff.dot(forward);
			r32 rope_interval = ROPE_SEGMENT_LENGTH / (1.0f + slack);
			Vec3 scale = Vec3(ROPE_RADIUS, ROPE_RADIUS, ROPE_SEGMENT_LENGTH * 0.5f);

			if (length > rope_interval * 0.5f)
			{
				if (last_segment->has<Rope>())
					Net::finalize(last_segment->entity());

				Vec3 spawn_pos = last_segment_pos + (diff / length) * rope_interval * 0.5f;
				Entity* box = World::create<PhysicsEntity>(AssetNull, spawn_pos, rot, RigidBody::Type::CapsuleZ, Vec3(ROPE_RADIUS, ROPE_SEGMENT_LENGTH - ROPE_RADIUS * 2.0f, 0.0f), 0.05f, CollisionDroneIgnore, ~CollisionWalker & ~CollisionMinionMoving & ~CollisionAllTeamsForceField);
				box->add<Rope>()->flags = flags;

				static Quat rotation_a = Quat::look(Vec3(0, 0, 1)) * Quat::euler(0, PI * -0.5f, 0);
				static Quat rotation_b = Quat::look(Vec3(0, 0, -1)) * Quat::euler(PI, PI * -0.5f, 0);

				RigidBody::Constraint constraint = RigidBody::Constraint();
				constraint.type = constraint_type;
				constraint.frame_a = btTransform(rotation_b, Vec3(0, 0, ROPE_SEGMENT_LENGTH * -0.5f));
				constraint.frame_b = btTransform(rotation_a, last_segment_relative_pos),
				constraint.limits = Vec3(PI, PI, 0);
				constraint.a = box->get<RigidBody>(); // this must be constraint A in order for the netcode to pick up on the constraint
				constraint.b = last_segment;
				RigidBody::add_constraint(constraint);

				box->get<RigidBody>()->set_ccd(true);
				box->get<RigidBody>()->set_damping(0.5f, 0.5f);
				last_segment = box->get<RigidBody>();
				last_segment_relative_pos = Vec3(0, 0, ROPE_SEGMENT_LENGTH * 0.5f);
			}
			else
				break;
		}
		else
			break;
	}

	if (last_segment == start) // we didn't add any rope segments
		return nullptr;
	else
		return last_segment;
}

Rope* Rope::start(RigidBody* start, const Vec3& abs_pos, const Vec3& abs_normal, const Quat& abs_rot, r32 slack, s8 flags)
{
	Entity* base = World::create<Prop>(Asset::Mesh::rope_base);
	base->get<Transform>()->absolute(abs_pos, Quat::look(abs_normal));
	base->get<Transform>()->reparent(start->get<Transform>());
	Net::finalize(base);

	// add the first rope segment
	Vec3 p = abs_pos + abs_normal * ROPE_RADIUS;
	Transform* start_trans = start->get<Transform>();
	RigidBody* rope = rope_add(start, start_trans->to_local(p), p + abs_rot * Vec3(0, 0, ROPE_SEGMENT_LENGTH), abs_rot, slack, RigidBody::Constraint::Type::PointToPoint, flags);
	vi_assert(rope); // should never happen
	return rope->get<Rope>();
}

void Rope::end(const Vec3& pos, const Vec3& normal, RigidBody* end, r32 slack, b8 add_cap)
{
	Vec3 abs_pos = pos + normal * ROPE_RADIUS;
	RigidBody* start = get<RigidBody>();
	Vec3 start_relative_pos = Vec3(0, 0, ROPE_SEGMENT_LENGTH * 0.5f);
	RigidBody* last = rope_add(start, start_relative_pos, abs_pos, Quat::look(Vec3::normalize(abs_pos - get<Transform>()->to_world(start_relative_pos))), slack, RigidBody::Constraint::Type::ConeTwist, flags);
	if (!last) // we didn't need to add any rope segments; just attach ourselves to the end point
		last = start;

	if (add_cap)
	{
		Entity* base = World::create<Prop>(Asset::Mesh::rope_base);
		base->get<Transform>()->absolute(pos, Quat::look(normal));
		base->get<Transform>()->reparent(end->get<Transform>());
		Net::finalize(base);
	}

	RigidBody::Constraint constraint = RigidBody::Constraint();
	constraint.type = RigidBody::Constraint::Type::PointToPoint;
	constraint.frame_a = btTransform(Quat::identity, start_relative_pos);
	constraint.frame_b = btTransform(Quat::identity, end->get<Transform>()->to_local(abs_pos));
	constraint.a = last;
	constraint.b = end; // this must be constraint A in order for the netcode to pick up on the constraint
	RigidBody::add_constraint(constraint);

	Net::finalize(last->entity());
}

void Rope::spawn(const Vec3& pos, const Vec3& dir, r32 max_distance, r32 slack, b8 attach_end, s8 flags)
{
	Vec3 dir_normalized = Vec3::normalize(dir);
	Vec3 start_pos = pos;
	Vec3 end = start_pos + dir_normalized * max_distance;
	btCollisionWorld::ClosestRayResultCallback ray_callback(start_pos, end);
	Physics::raycast(&ray_callback, btBroadphaseProxy::AllFilter & ~CollisionAllTeamsForceField);
	if (ray_callback.hasHit())
	{
		Vec3 end2 = start_pos + dir_normalized * -max_distance;

		btCollisionWorld::ClosestRayResultCallback ray_callback2(start_pos, end2);
		Physics::raycast(&ray_callback2, btBroadphaseProxy::AllFilter & ~CollisionAllTeamsForceField);

		if (!attach_end || ray_callback2.hasHit())
		{
			RigidBody* a = Entity::list[ray_callback.m_collisionObject->getUserIndex()].get<RigidBody>();

			Rope* rope = Rope::start(a, ray_callback.m_hitPointWorld, ray_callback.m_hitNormalWorld, Quat::look(ray_callback.m_hitNormalWorld), slack, flags);

			if (rope)
			{
				if (attach_end && ray_callback2.hasHit())
				{
					// attach on both ends
					RigidBody* b = Entity::list[ray_callback2.m_collisionObject->getUserIndex()].get<RigidBody>();
					rope->end(ray_callback2.m_hitPointWorld, ray_callback2.m_hitNormalWorld, b, slack);
				}
				else
				{
					// only attached on one end
					Vec3 start_relative_pos = Vec3(0, 0, ROPE_SEGMENT_LENGTH * 0.5f);
					rope_add(rope->get<RigidBody>(), start_relative_pos, start_pos, Quat::look(Vec3::normalize(start_pos - rope->get<Transform>()->to_world(start_relative_pos))), slack, RigidBody::Constraint::Type::ConeTwist, flags);
				}
			}
		}
	}
}

WaterEntity::WaterEntity(AssetID mesh_id)
{
	create<Transform>();
	create<Water>(mesh_id);
}

PinArray<EffectLight, MAX_ENTITIES> EffectLight::list;

EffectLight* EffectLight::add(const Vec3& pos, r32 radius, r32 duration, Type t, Transform* parent, Quat rot)
{
	EffectLight* s = list.add();
	s->rot = rot;
	s->pos = parent ? parent->to_local(pos) : pos;
	s->max_radius = radius;
	s->duration = duration;
	s->type = t;
	s->parent = parent;
	s->revision++;
	s->timer = 0.0f;
	return s;
}

void EffectLight::remove(EffectLight* s)
{
	s->revision++;
	list.remove(s->id());
}

void EffectLight::draw_alpha(const RenderParams& params)
{
	// "Light" and "Wave" type shockwaves get rendered in loop.h, not here
	Loader::mesh_permanent(Asset::Mesh::sphere_highres);
	Loader::shader_permanent(Asset::Shader::fresnel);

	RenderSync* sync = params.sync;
	sync->write(RenderOp::Shader);
	sync->write(Asset::Shader::fresnel);
	sync->write(params.technique);

	for (auto i = list.iterator(); !i.is_last(); i.next())
	{
		r32 radius = i.item()->radius();
		Vec3 pos = i.item()->absolute_pos();
		if ((i.item()->type == Type::Explosion || i.item()->type == Type::MuzzleFlash) && params.camera->visible_sphere(pos, radius))
		{
			Mat4 m;
			m.make_transform(pos, Vec3(radius), Quat::identity);
			Mat4 mvp = m * params.view_projection;

			sync->write(RenderOp::Uniform);
			sync->write(Asset::Uniform::mvp);
			sync->write(RenderDataType::Mat4);
			sync->write<s32>(1);
			sync->write<Mat4>(mvp);

			sync->write(RenderOp::Uniform);
			sync->write(Asset::Uniform::diffuse_color);
			sync->write(RenderDataType::Vec4);
			sync->write<s32>(1);
			sync->write<Vec4>(Vec4(1, 1, 1, i.item()->opacity()));

			sync->write(RenderOp::Mesh);
			sync->write(RenderPrimitiveMode::Triangles);
			sync->write(Asset::Mesh::sphere_highres);
		}
	}
}

void EffectLight::draw_opaque(const RenderParams& params)
{
	for (auto i = list.iterator(); !i.is_last(); i.next())
	{
		if (i.item()->type == Type::Grenade)
		{
			Mat4 m;

			Vec3 initial_velocity = i.item()->rot * Vec3(0, 0, GRENADE_LAUNCH_SPEED);
			Vec3 p = i.item()->pos + (initial_velocity * i.item()->timer) + (Vec3(Physics::btWorld->getGravity()) * ((i.item()->timer * i.item()->timer) * 0.5f));
			m.make_transform(p, Vec3(GRENADE_RADIUS), i.item()->rot);
			View::draw_mesh(params, Asset::Mesh::grenade_detached, Asset::Shader::standard, AssetNull, m, Vec4(1, 1, 1, MATERIAL_NO_OVERRIDE), GRENADE_RADIUS);
		}
	}
}

r32 EffectLight::radius() const
{
	switch (type)
	{
		case Type::BoltDroneBolter:
		case Type::Grenade:
			return max_radius;
		case Type::BoltDroneShotgun:
			return 0.0f;
		case Type::Shockwave:
			return Ease::cubic_out(timer / duration, 0.0f, max_radius);
		case Type::Spark:
		{
			r32 blend = Ease::cubic_in<r32>(timer / duration);
			return LMath::lerpf(blend, max_radius * 0.25f, max_radius);
		}
		case Type::Explosion:
		{
			r32 blend = Ease::cubic_in<r32>(timer / duration);
			return LMath::lerpf(blend, 0.0f, max_radius);
		}
		case Type::MuzzleFlash:
		{
			r32 blend = timer / duration;
			if (blend < 0.2f)
				return LMath::lerpf(Ease::cubic_out<r32>(blend / 0.2f), 0.0f, max_radius);
			else
				return LMath::lerpf(Ease::cubic_in<r32>((blend - 0.2f) / (0.8f)), max_radius, 0.0f);
		}
		default:
		{
			vi_assert(false);
			return 0.0f;
		}
	}
}

r32 EffectLight::opacity() const
{
	switch (type)
	{
		case Type::BoltDroneBolter:
		case Type::BoltDroneShotgun:
		case Type::MuzzleFlash:
		case Type::Grenade:
			return 1.0f;
		case Type::Spark:
		{
			r32 blend = Ease::cubic_in<r32>(timer / duration);
			return LMath::lerpf(blend, 1.0f, 0.0f);
		}
		case Type::Shockwave:
		{
			r32 fade_radius = max_radius * (2.0f / 15.0f);
			r32 fade = 1.0f - vi_max(0.0f, ((radius() - (max_radius - fade_radius)) / fade_radius));
			return fade * 0.8f;
		}
		case Type::Explosion:
		{
			r32 blend = Ease::cubic_in<r32>(timer / duration);
			return LMath::lerpf(blend, 0.8f, 0.0f);
		}
		default:
		{
			vi_assert(false);
			return 0.0f;
		}
	}
}

Vec3 EffectLight::absolute_pos() const
{
	Transform* p = parent.ref();
	return p ? p->to_world(pos) : pos;
}

void EffectLight::update(const Update& u)
{
	timer += u.time.delta;
	if (timer > duration)
		remove(this);
	else if (type == Type::BoltDroneBolter)
		pos += rot * Vec3(0, 0, u.time.delta * BOLT_SPEED_DRONE_BOLTER);
	else if (type == Type::BoltDroneShotgun)
		pos += rot * Vec3(0, 0, u.time.delta * BOLT_SPEED_DRONE_SHOTGUN);
}

CollectibleEntity::CollectibleEntity(ID save_id, Resource type, s16 amount)
{
	create<Transform>();

	PointLight* light = create<PointLight>();
	light->radius = 6.0f;
	light->offset = Vec3(0, 0, 0.2f);
	light->color = Team::color_neutral().xyz();

	Collectible* c = create<Collectible>();
	c->save_id = save_id;
	c->type = type;
	c->amount = amount;
	switch (type)
	{
		case Resource::Energy:
		case Resource::AudioLog:
		{
			// simple models
			AssetID mesh;
			if (type == Resource::Energy)
				mesh = Asset::Mesh::energy;
			else
				mesh = Asset::Mesh::audio_log;
			View* v = create<View>(mesh);
			v->shader = Asset::Shader::standard;
			v->color = Team::color_neutral();
			break;
		}
		default:
			vi_assert(false);
			break;
	}
}

void Collectible::give_rewards()
{
	s16 a = amount;
	if (a == 0)
	{
		switch (type)
		{
			case Resource::AudioLog:
				a = 1;
				break;
			case Resource::Energy:
				a = 100;
				break;
			default:
				vi_assert(false);
				break;
		}
	}
	if (Game::level.local)
		Overworld::resource_change(type, a);

	if (type == Resource::AudioLog)
	{
		PlayerHuman* player = PlayerHuman::list.iterator().item();
		player->audio_log_pickup(audio_log);
	}

	Game::save.collectibles.add({ Game::level.id, save_id });

	char msg[512];
	sprintf(msg, _(strings::resource_collected), a, _(Overworld::resource_info[s32(type)].description));
	for (auto i = PlayerHuman::list.iterator(); !i.is_last(); i.next())
		i.item()->msg(msg, PlayerHuman::FlagMessageGood);
	collected.fire();
}

Interactable* Interactable::closest(const Vec3& pos)
{
	r32 distance_sq = 1.25f * 1.25f;
	Interactable* result = nullptr;
	// find the closest interactable
	for (auto i = list.iterator(); !i.is_last(); i.next())
	{
		Vec3 i_pos(0.4f, 1.1f, 0);
		Quat i_rot = Quat::identity;
		i.item()->get<Transform>()->to_world(&i_pos, &i_rot);
		Vec3 to_interactable = i_pos - pos;
		r32 d = to_interactable.length_squared();
		if (d < distance_sq)
		{
			distance_sq = d;
			result = i.item();
		}
	}
	return result;
}

Interactable::Interactable(Type t)
	: type(t),
	user_data(),
	interacted()
{
}

void Interactable::awake()
{
	if (has<Animator>())
	{
		link<&Interactable::animation_callback>(get<Animator>()->trigger(Asset::Animation::interactable_interact, 1.916f));
		link<&Interactable::animation_callback>(get<Animator>()->trigger(Asset::Animation::interactable_interact_disable, 1.916f));
	}

	switch (type)
	{
		case Type::Terminal:
			interacted.link(&TerminalInteractableEntity::interacted);
			break;
		case Type::Tram:
			interacted.link(&TramInteractableEntity::interacted);
			break;
		case Type::Shop:
			interacted.link(&ShopInteractableEntity::interacted);
			break;
		default:
			vi_assert(false);
			break;
	}
}

void Interactable::interact()
{
	Animator* anim = get<Animator>();
	if (anim->layers[1].animation == AssetNull)
	{
		if (type == Type::Terminal)
		{
			anim->layers[0].animation = Asset::Animation::interactable_disabled;
			anim->layers[1].play(Asset::Animation::interactable_interact_disable);
		}
		else
		{
			anim->layers[0].animation = Asset::Animation::interactable_enabled;
			anim->layers[1].play(Asset::Animation::interactable_interact);
		}
	}
}

void Interactable::interact_no_animation()
{
	interacted.fire(this);
}

void Interactable::animation_callback()
{
	interacted.fire(this);
}

b8 Interactable::is_present(Type t)
{
	for (auto i = list.iterator(); !i.is_last(); i.next())
	{
		if (i.item()->type == t)
			return true;
	}
	return false;
}

ShopEntity::ShopEntity()
{
	create<Transform>();

	View* model = create<View>();
	model->mesh = Asset::Mesh::shop_view;
	model->shader = Asset::Shader::standard;
	model->color = Team::color_neutral();

	RigidBody* body = create<RigidBody>(RigidBody::Type::Mesh, Vec3::zero, 0.0f, CollisionStatic | CollisionInaccessible, ~CollisionStatic & ~CollisionAudio & ~CollisionParkour & ~CollisionInaccessible & ~CollisionElectric & ~CollisionGlass, Asset::Mesh::shop_collision);
	body->set_restitution(0.75f);
}

ShopInteractableEntity::ShopInteractableEntity()
{
	create<Transform>();
	create<Interactable>(Interactable::Type::Shop);
}

void ShopInteractableEntity::interacted(Interactable*)
{
}

void TerminalEntity::open()
{
	Animator* animator = Game::level.terminal.ref()->get<Animator>();
	animator->layers[0].play(Asset::Animation::terminal_opened);
	animator->layers[1].play(Asset::Animation::terminal_open);
	Audio::post_global(AK::EVENTS::PLAY_TERMINAL_OPEN, Game::level.terminal.ref()->get<Transform>()->absolute_pos());
}

void TerminalEntity::close()
{
	Animator* animator = Game::level.terminal.ref()->get<Animator>();
	animator->layers[0].animation = AssetNull;
	animator->layers[1].play(Asset::Animation::terminal_close);
	Audio::post_global(AK::EVENTS::PLAY_TERMINAL_CLOSE, Game::level.terminal.ref()->get<Transform>()->absolute_pos());
}

void TerminalEntity::closed()
{
	if (Game::level.local)
		Overworld::show(PlayerHuman::list.iterator().item()->camera.ref(), Overworld::State::StoryMode);
}

TerminalEntity::TerminalEntity()
{
	Transform* transform = create<Transform>();
	
	SkinnedModel* model = create<SkinnedModel>();
	model->color = Team::color_neutral();
	model->mesh = Asset::Mesh::terminal;
	model->shader = Asset::Shader::armature;

	Animator* anim = create<Animator>();
	anim->armature = Asset::Armature::terminal;
	anim->layers[0].behavior = Animator::Behavior::Loop;
	anim->layers[0].blend_time = 0.0f;
	anim->layers[0].animation = (Game::save.zones[Game::level.id] == ZoneState::ParkourOwned && !Game::save.inside_terminal) ? Asset::Animation::terminal_opened : AssetNull;
	anim->layers[1].blend_time = 0.0f;
	anim->trigger(Asset::Animation::terminal_close, 1.33f).link(&closed);

	RigidBody* body = create<RigidBody>(RigidBody::Type::Mesh, Vec3::zero, 0.0f, CollisionStatic | CollisionInaccessible, ~CollisionStatic & ~CollisionAudio & ~CollisionParkour & ~CollisionInaccessible & ~CollisionElectric & ~CollisionGlass, Asset::Mesh::terminal_collision);
	body->set_restitution(0.75f);
}

TerminalInteractableEntity::TerminalInteractableEntity()
{
	Transform* transform = create<Transform>();

	SkinnedModel* model = create<SkinnedModel>();
	model->mesh = Asset::Mesh::interactable;
	model->shader = Asset::Shader::armature;
	model->alpha();

	Animator* anim = create<Animator>();
	anim->armature = Asset::Armature::interactable;
	anim->layers[0].behavior = Animator::Behavior::Loop;
	anim->layers[0].animation = Game::save.zones[Game::level.id] == ZoneState::ParkourOwned ? Asset::Animation::interactable_disabled : Asset::Animation::interactable_enabled;
	anim->layers[0].blend_time = 0.0f;
	anim->layers[1].blend_time = 0.0f;

	create<Interactable>(Interactable::Type::Terminal);
}

void TerminalInteractableEntity::interacted(Interactable* i)
{
	vi_assert(Game::level.mode == Game::Mode::Parkour);

	Animator* animator = Game::level.terminal.ref()->get<Animator>();
	if (animator->layers[1].animation == AssetNull) // make sure nothing's happening already
	{
		ZoneState zone_state = Game::save.zones[Game::level.id];
		if (zone_state == ZoneState::ParkourOwned) // already open; get in
			TerminalEntity::close();
		else // open up
		{
			Overworld::zone_change(Game::level.id, ZoneState::ParkourOwned);
			Overworld::zone_change(AssetID(i->user_data), ZoneState::PvpUnlocked); // user data = target PvP level
			for (auto i = PlayerHuman::list.iterator(); !i.is_last(); i.next())
				i.item()->msg(_(strings::terminal_hacked), PlayerHuman::FlagMessageGood);
			TerminalEntity::open();
		}
	}
}

const r32 TRAM_LENGTH = 3.7f * 2.0f;
const r32 TRAM_SPEED_MAX = 10.0f;
const r32 TRAM_WIDTH = TRAM_LENGTH * 0.5f;
const r32 TRAM_HEIGHT = 2.54f;
const r32 TRAM_ROPE_LENGTH = 4.0f;

TramRunnerEntity::TramRunnerEntity(s8 track, b8 is_front)
{
	create<Transform>();
	View* model = create<View>(Asset::Mesh::tram_runner);
	model->shader = Asset::Shader::standard;
	model->color = Team::color_neutral();
	RigidBody* body = create<RigidBody>(RigidBody::Type::Mesh, Vec3::zero, 0.0f, CollisionStatic | CollisionInaccessible, ~CollisionStatic & ~CollisionAudio & ~CollisionInaccessible & ~CollisionParkour & ~CollisionElectric & ~CollisionGlass, Asset::Mesh::tram_runner);
	body->set_restitution(0.75f);
	TramRunner* r = create<TramRunner>();
	r->track = track;
	r->is_front = is_front;

	create<Audio>();

	const Game::TramTrack& t = Game::level.tram_tracks[track];
	r32 offset;
	if (Game::save.zone_last == t.level)
	{
		offset = t.points[t.points.length - 1].offset - TRAM_LENGTH;
		r->velocity = -TRAM_SPEED_MAX;
		r->state = TramRunner::State::Arriving;
	}
	else
		offset = 0.0f;

	if (is_front)
	{
		offset += TRAM_LENGTH;
		r->target_offset += TRAM_LENGTH;
	}
	r->set(offset);
}

void TramRunner::set(r32 x)
{
	const Game::TramTrack& t = Game::level.tram_tracks[track];
	while (true)
	{
		const Game::TramTrack::Point current = t.points[offset_index];
		if (offset_index > 0 && x < current.offset)
			offset_index--;
		else if (offset_index < t.points.length - 2 && t.points[offset_index + 1].offset < x)
			offset_index++;
		else
			break;
	}

	const Game::TramTrack::Point current = t.points[offset_index];
	const Game::TramTrack::Point next = t.points[offset_index + 1];
	r32 blend = (x - current.offset) / (next.offset - current.offset);
	Transform* transform = get<Transform>();
	transform->pos = Vec3::lerp(blend, current.pos, next.pos);
	transform->rot = Quat::look(Vec3::normalize(next.pos - current.pos));
	offset = x;
}

void TramRunner::go(s8 track, r32 x, State s)
{
	vi_assert(Game::level.local);
	const Game::TramTrack& t = Game::level.tram_tracks[track];
	for (auto i = list.iterator(); !i.is_last(); i.next())
	{
		if (i.item()->track == track)
		{
			i.item()->state = s;
			i.item()->target_offset = x * (t.points[t.points.length - 1].offset - TRAM_LENGTH);
			if (i.item()->is_front)
				i.item()->target_offset += TRAM_LENGTH;
		}
	}
}

void TramRunner::awake()
{
}

void TramRunner::update(const Update& u)
{
	const r32 ACCEL_TIME = 5.0f;
	const r32 ACCEL_MAX = TRAM_SPEED_MAX / ACCEL_TIME;
	const r32 ACCEL_DISTANCE = TRAM_SPEED_MAX * ACCEL_TIME - 0.5f * ACCEL_MAX * (ACCEL_TIME * ACCEL_TIME);

	{
		const Game::TramTrack& t = Game::level.tram_tracks[track];
		if (is_front
			&& state == State::Departing
			&& Game::scheduled_load_level == AssetNull
			&& offset > t.points[t.points.length - 1].offset) // we hit our goal, we are the front runner, we're a local game, and we're exiting the level
		{
#if RELEASE_BUILD
			Game::scheduled_dialog = strings::demo_end;
			Menu::title();
#else
			Game::schedule_load_level(t.level, Game::Mode::Parkour);
#endif
		}
	}

	r32 error = target_offset - offset;
	r32 distance = fabsf(error);
	r32 dv_half = ACCEL_MAX * u.time.delta * 0.5f;
	if (state == State::Departing || distance > ACCEL_DISTANCE) // accelerating to max speed
	{
		get<RigidBody>()->activate_linked();
		if (error > 0.0f)
			velocity = vi_min(TRAM_SPEED_MAX, velocity + dv_half);
		else
			velocity = vi_max(-TRAM_SPEED_MAX, velocity - dv_half);
		set(offset + velocity * u.time.delta);
		if (error > 0.0f)
			velocity = vi_min(TRAM_SPEED_MAX, velocity + dv_half);
		else
			velocity = vi_max(-TRAM_SPEED_MAX, velocity - dv_half);
	}
	else if (distance == 0.0f) // stopped
	{
		if (state == State::Arriving)
		{
			if (is_front)
				Tram::by_track(track)->doors_open(true);
			state = State::Idle;
		}

		get<RigidBody>()->btBody->setActivationState(ISLAND_SLEEPING);
	}
	else // decelerating
	{
		if (velocity > 0.0f)
		{
			velocity = vi_max(0.01f, velocity - dv_half);
			set(vi_min(target_offset, offset + velocity * u.time.delta));
		}
		else
		{
			velocity = vi_min(-0.01f, velocity + dv_half);
			set(vi_max(target_offset, offset + velocity * u.time.delta));
		}
		if (velocity > 0.0f)
			velocity = vi_max(0.01f, velocity - dv_half);
		else
			velocity = vi_min(-0.01f, velocity + dv_half);

		if (is_front && distance >= ACCEL_DISTANCE * 0.25f && fabsf(target_offset - offset) < ACCEL_DISTANCE * 0.25f)
		{
			Tram* tram = Tram::by_track(track);
			tram->get<Audio>()->stop(AK::EVENTS::STOP_TRAM_LOOP);
			tram->get<Audio>()->post(AK::EVENTS::PLAY_TRAM_STOP);
		}
	}

	if (velocity != 0.0f && mersenne::randf_co() < u.time.delta / 3.0f)
	{
		b8 left = mersenne::randf_co() < 0.5f;
		Vec3 pos = get<Transform>()->to_world(Vec3(left ? -0.35f : 0.35f, 0.45f, 0));

		Quat rot = get<Transform>()->absolute_rot() * Quat::euler(0, (left ? PI * -0.5f : PI * 0.5f), 0);
		for (s32 i = 0; i < 15; i++)
		{
			Particles::sparks.add
			(
				pos,
				rot * Vec3(mersenne::randf_oo() * 2.0f - 1.0f, mersenne::randf_oo() * 2.0f - 1.0f, mersenne::randf_oo()) * 10.0f,
				Vec4(1, 1, 1, 1)
			);
		}

		get<Audio>()->post(AK::EVENTS::PLAY_TRAM_SPARK);
		EffectLight::add(pos + Vec3(0, -0.2f, 0), 3.0f, 0.25f, EffectLight::Type::Spark, get<Transform>());
	}
}

TramEntity::TramEntity(TramRunner* runner_a, TramRunner* runner_b)
{
	Transform* transform = create<Transform>();

	const Mesh* mesh = Loader::mesh(Asset::Mesh::tram_mesh);
	RigidBody* body = create<RigidBody>(RigidBody::Type::Box, (mesh->bounds_max - mesh->bounds_min) * 0.5f, 5.0f, CollisionDroneIgnore, ~CollisionWalker & ~CollisionMinionMoving & ~CollisionInaccessible & ~CollisionParkour & ~CollisionStatic & ~CollisionAudio & ~CollisionElectric & ~CollisionGlass);
	body->set_restitution(0.75f);
	body->set_damping(0.5f, 0.5f);

	create<Audio>();

	Tram* tram = create<Tram>();
	tram->runner_a = runner_a->get<TramRunner>();
	tram->runner_b = runner_b->get<TramRunner>();

	tram->set_position();
	body->rebuild();

	create<PlayerTrigger>()->radius = 1.3f; // trigger for entering

	View* view = create<View>();
	view->mesh = Asset::Mesh::tram_mesh;
	view->shader = Asset::Shader::standard;
	view->color = Team::color_neutral();

	// glass
	{
		Entity* child = World::alloc<StaticGeom>(Asset::Mesh::tram_collision, Vec3::zero, Quat::identity, CollisionInaccessible, ~CollisionDroneIgnore & ~CollisionInaccessible & ~CollisionParkour & ~CollisionElectric);
		child->get<Transform>()->parent = transform;
		child->get<View>()->mesh = Asset::Mesh::tram_mesh_1;
		child->get<View>()->shader = Asset::Shader::flat;
		child->get<View>()->alpha();
		World::awake(child);
		Net::finalize_child(child);
	}

	// doors
	{
		Entity* doors = World::alloc<Empty>();
		doors->get<Transform>()->parent = transform;
		doors->get<Transform>()->rot = Quat::identity;

		SkinnedModel* model = doors->create<SkinnedModel>();
		model->mesh = Asset::Mesh::tram_doors;
		model->shader = Asset::Shader::armature;
		model->color = Team::color_neutral();

		Animator* anim = doors->create<Animator>();
		anim->armature = Asset::Armature::tram_doors;
		anim->layers[0].behavior = Animator::Behavior::Loop;
		anim->layers[0].blend_time = 0.0f;
		anim->layers[1].blend_time = 0.0f;

		RigidBody* body = doors->create<RigidBody>(RigidBody::Type::Mesh, Vec3::zero, 0.0f, CollisionStatic | CollisionInaccessible, ~CollisionStatic & ~CollisionAudio & ~CollisionDroneIgnore & ~CollisionInaccessible & ~CollisionParkour & ~CollisionElectric & ~CollisionGlass, Asset::Mesh::tram_collision_door);
		body->set_restitution(0.75f);
		
		doors->create<PlayerTrigger>()->radius = 8.0f; // trigger for exiting

		World::awake(doors);
		Net::finalize_child(doors);

		tram->doors = doors;
	}

	{
		runner_a->get<RigidBody>()->rebuild();
		Quat rot_a = runner_a->get<Transform>()->rot;
		Rope* rope1 = Rope::start(runner_a->get<RigidBody>(), runner_a->get<Transform>()->pos + rot_a * Vec3(0, -0.37f, 0), rot_a * Vec3(0, -1, 0), Quat::look(rot_a * Vec3(0, -1, 0)));
		if (rope1)
			rope1->end(transform->to_world(Vec3(0, TRAM_HEIGHT, -TRAM_WIDTH)), transform->rot * (Quat::euler(0, 0, PI * (30.0f / 180.0f)) * Vec3(0, 1, 0)), body, 0.0f, true);
	}
	{
		runner_b->get<RigidBody>()->rebuild();
		Quat rot_b = runner_b->get<Transform>()->rot;
		Rope* rope2 = Rope::start(runner_b->get<RigidBody>(), runner_b->get<Transform>()->pos + rot_b * Vec3(0, -0.37f, 0), rot_b * Vec3(0, -1, 0), Quat::look(rot_b * Vec3(0, -1, 0)));
		if (rope2)
			rope2->end(transform->to_world(Vec3(0, TRAM_HEIGHT, TRAM_WIDTH)), rot_b * (Quat::euler(0, 0, PI * (-30.0f / 180.0f)) * Vec3(0, 1, 0)), body, 0.0f, true);
	}
}

void Tram::awake()
{
	link_arg<Entity*, &Tram::player_entered>(get<PlayerTrigger>()->entered);
	link_arg<Entity*, &Tram::player_exited>(doors.ref()->get<PlayerTrigger>()->exited);
	if (runner_b.ref()->state == TramRunner::State::Arriving)
		get<Audio>()->post(AK::EVENTS::PLAY_TRAM_LOOP);
}

void Tram::setup()
{
	for (auto i = list.iterator(); !i.is_last(); i.next())
		i.item()->set_position();
}

void Tram::set_position()
{
	Vec3 pos_a = runner_a.ref()->get<Transform>()->pos;
	Vec3 pos_b = runner_b.ref()->get<Transform>()->pos;
	Transform* transform = get<Transform>();
	transform->pos = (pos_a + pos_b) * 0.5f;
	transform->pos.y += (fabsf(pos_b.y - pos_a.y) * 0.5f) - TRAM_HEIGHT - TRAM_ROPE_LENGTH;
	transform->rot = Quat::look(Vec3::normalize(pos_b - pos_a));
	if (runner_b.ref()->state == TramRunner::State::Arriving)
	{
		get<RigidBody>()->awake(); // create the rigid body if we haven't yet
		get<RigidBody>()->btBody->setLinearVelocity(transform->rot * Vec3(0, 0, -TRAM_SPEED_MAX));
	}
}

void Tram::player_entered(Entity* e)
{
	if (e->has<Parkour>() && e->get<Parkour>()->fsm.current != ParkourState::Grapple)
	{
		if (departing && doors_open()) // close doors and depart
		{
			doors_open(false);
			Tram::by_track(track())->get<Audio>()->post(AK::EVENTS::PLAY_TRAM_START);
			Tram::by_track(track())->get<Audio>()->post(AK::EVENTS::PLAY_TRAM_LOOP_DELAYED);
			TramRunner::go(track(), 1.0f, TramRunner::State::Departing);
		}
		else if (runner_a.ref()->state == TramRunner::State::Idle && !doors_open())
		{
			// player spawned inside us and we're sitting still
			// open the doors for them
			doors_open(true);
		}
	}
}

void Tram::player_exited(Entity* e)
{
	if (!departing
		&& doors_open()
		&& e->has<Parkour>()
		&& e->get<PlayerControlHuman>()->local())
	{
		doors_open(false);
		departing = false;
	}
}

Tram* Tram::by_track(s8 track)
{
	for (auto i = list.iterator(); !i.is_last(); i.next())
	{
		if (i.item()->runner_b.ref()->track == track)
			return i.item();
	}
	return nullptr;
}

Tram* Tram::player_inside(Entity* player)
{
	Ref<RigidBody> support = player->get<Walker>()->support;
	for (auto i = list.iterator(); !i.is_last(); i.next())
	{
		if (support.equals(i.item()->get<RigidBody>()))
			return i.item();
	}
	return nullptr;
}

s8 Tram::track() const
{
	return runner_b.ref()->track;
}

b8 Tram::doors_open() const
{
	return doors.ref()->get<RigidBody>()->collision_filter == 0;
}

void Tram::doors_open(b8 open)
{
	Animator* anim = doors.ref()->get<Animator>();
	RigidBody* body = doors.ref()->get<RigidBody>();
	if (open)
	{
		body->set_collision_masks(CollisionStatic | CollisionInaccessible, 0); // disable collision
		anim->layers[0].play(Asset::Animation::tram_doors_opened);
		anim->layers[1].play(Asset::Animation::tram_doors_open);
		get<Audio>()->post(AK::EVENTS::PLAY_TRAM_OPEN);
	}
	else
	{
		body->set_collision_masks(CollisionStatic | CollisionInaccessible, ~CollisionStatic & ~CollisionAudio & ~CollisionDroneIgnore & ~CollisionParkour & ~CollisionInaccessible & ~CollisionElectric & ~CollisionGlass); // enable collision
		anim->layers[0].animation = AssetNull;
		anim->layers[1].play(Asset::Animation::tram_doors_close);
		get<Audio>()->post(AK::EVENTS::PLAY_TRAM_CLOSE);
	}
}

void TramInteractableEntity::interacted(Interactable* i)
{
	s8 track = s8(i->user_data);
	Tram* tram = Tram::by_track(track);
	if (tram->doors_open())
	{
		tram->departing = false;
		tram->doors_open(false);
	}
	else
	{
		AssetID target_level = Game::level.tram_tracks[track].level;
		if (Game::save.zones[target_level] == ZoneState::Locked)
			Overworld::zone_change(target_level, ZoneState::ParkourUnlocked);
		tram->departing = true;
		tram->doors_open(true);
	}
}

TramInteractableEntity::TramInteractableEntity(const Vec3& absolute_pos, const Quat& absolute_rot, s8 track)
{
	create<Transform>();

	SkinnedModel* model = create<SkinnedModel>();
	model->mesh = Asset::Mesh::interactable;
	model->shader = Asset::Shader::armature;
	model->alpha();

	Animator* anim = create<Animator>();
	anim->armature = Asset::Armature::interactable;
	anim->layers[0].behavior = Animator::Behavior::Loop;
	anim->layers[0].animation = Asset::Animation::interactable_enabled;
	anim->layers[0].blend_time = 0.0f;
	anim->layers[1].blend_time = 0.0f;

	Interactable* i = create<Interactable>(Interactable::Type::Tram);
	i->user_data = track;

	{
		Entity* collision = World::create<StaticGeom>(Asset::Mesh::interactable_collision, absolute_pos, absolute_rot, CollisionInaccessible, ~CollisionParkour & ~CollisionInaccessible & ~CollisionElectric);
		collision->get<View>()->color.w = MATERIAL_INACCESSIBLE;
		Net::finalize_child(collision);
	}
}

Array<Ascensions::Entry> Ascensions::list;
r32 Ascensions::timer;
r32 Ascensions::particle_accumulator;

const r32 ascension_total_time = 20.0f;

Vec3 Ascensions::Entry::pos() const
{
	r32 blend = 1.0f - (timer / ascension_total_time);
	return Quat::euler(Ease::circ_out<r32>(blend) * PI * 0.45f, Game::level.rotation, 0) * Vec3(Game::level.far_plane_get() * 0.9f, 0, 0);
}

r32 Ascensions::Entry::scale() const
{
	r32 blend = 1.0f - (timer / ascension_total_time);
	return (Game::level.far_plane_get() / 100.0f) * LMath::lerpf(blend, 1.0f, 0.5f);
}

void Ascensions::add()
{
	Entry* e = list.add();
	e->timer = ascension_total_time;
}

void Ascensions::update(const Update& u)
{
	if (Game::level.mode != Game::Mode::Special)
	{
		timer -= u.real_time.delta;
		if (timer < 0.0f)
		{
			timer = 40.0f + mersenne::randf_co() * 200.0f;
			add();
		}
	}

	for (s32 i = 0; i < list.length; i++)
	{
		Entry* e = &list[i];
		r32 old_timer = e->timer;
		e->timer -= u.time.delta;
		if (e->timer < 0.0f)
		{
			list.remove(i);
			i--;
		}
	}

	// particles
	const r32 interval = 0.5f;
	particle_accumulator += u.time.delta;
	while (particle_accumulator > interval)
	{
		particle_accumulator -= interval;
		for (s32 i = 0; i < list.length; i++)
		{
			const Entry& e = list[i];
			Particles::tracers_skybox.add(e.pos(), e.scale());
		}
	}
}

void Ascensions::clear()
{
	timer = 40.0f + mersenne::randf_co() * 200.0f;
	list.length = 0;
}

Array<Asteroids::Entry> Asteroids::list;
r32 Asteroids::timer;
r32 Asteroids::particle_accumulator;
Array<InstanceVertex> Asteroids::instances;

void Asteroids::update(const Update& u)
{
	if (Game::level.asteroids > 0.0f && PlayerHuman::list.count() > 0)
	{
		timer -= u.time.delta;
		while (timer < 0.0f)
		{
			Camera* camera = PlayerHuman::list.iterator().item()->camera.ref();
			timer += LMath::lerpf(Game::level.asteroids, 180.0f, 0.5f);
			Entry* entry = list.add();
			entry->pos = camera->pos + Quat::euler((1.0f + mersenne::randf_cc()) * PI * 0.2f, mersenne::randf_cc() * PI * 2.0f, 0) * Vec3(Game::level.far_plane_get() * 0.8f, 0, 0);
			entry->velocity = Quat::euler(PI * -0.3f, Game::level.rotation, 0) * Vec3(20.0f + mersenne::randf_cc() * 15.0f, 0, 0);
		}
	}

	// remove old entries
	for (s32 i = 0; i < list.length; i++)
	{
		Entry* e = &list[i];
		e->lifetime += u.time.delta;
		e->pos += e->velocity * u.time.delta;
		if (e->pos.y < 20.0f)
		{
			list.remove(i);
			i--;
		}
	}

	// spawn particles
	particle_accumulator -= u.time.delta;
	while (particle_accumulator < 0.0f)
	{
		particle_accumulator += 0.2f;
		for (s32 i = 0; i < list.length; i++)
			Particles::tracers_skybox.add(list[i].pos, 2.0f);
	}
}

void Asteroids::draw_alpha(const RenderParams& params)
{
	if (!params.camera->mask || (params.flags & RenderFlagEdges))
		return;

	const Mesh* mesh_data = Loader::mesh_instanced(Asset::Mesh::asteroid);
	Vec3 radius = (Vec4(mesh_data->bounds_radius, mesh_data->bounds_radius, mesh_data->bounds_radius, 0)).xyz();
	r32 f_radius = vi_max(radius.x, vi_max(radius.y, radius.z));

	RenderSync* sync = params.sync;

	if (params.update_instances())
	{
		instances.length = 0;
		for (s32 i = 0; i < list.length; i++)
		{
			const Entry& entry = list[i];
			r32 scale = 0.03f * vi_min(entry.lifetime, 2.0f) * sqrtf((params.camera->pos - entry.pos).length());
			if (params.camera->visible_sphere(entry.pos, scale * f_radius))
			{
				InstanceVertex* instance = instances.add();
				instance->world_matrix.make_transform(entry.pos, Vec3(scale), Quat::identity);
				instance->color = Vec4(1);
			}
		}
	}

	if (instances.length == 0)
		return;

	Loader::shader_permanent(Asset::Shader::flat_instanced);

	sync->write(RenderOp::Shader);
	sync->write(Asset::Shader::flat_instanced);
	sync->write(params.technique);

	Mat4 vp = params.view_projection;

	sync->write(RenderOp::Uniform);
	sync->write(Asset::Uniform::vp);
	sync->write(RenderDataType::Mat4);
	sync->write<s32>(1);
	sync->write<Mat4>(vp);

	sync->write(RenderOp::Uniform);
	sync->write(Asset::Uniform::v);
	sync->write(RenderDataType::Mat4);
	sync->write<s32>(1);
	sync->write<Mat4>(params.view);

	sync->write(RenderOp::Uniform);
	sync->write(Asset::Uniform::diffuse_color);
	sync->write(RenderDataType::Vec4);
	sync->write<s32>(1);
	sync->write<Vec4>(Vec4(1, 1, 1, 1));

	sync->write(RenderOp::UpdateInstances);
	sync->write(Asset::Mesh::asteroid);
	sync->write(instances.length);
	sync->write<InstanceVertex>(instances.data, instances.length);

	sync->write(RenderOp::Instances);
	sync->write(Asset::Mesh::asteroid);
}

void Asteroids::clear()
{
	timer = 0.0f;
	list.length = 0;
}

PinArray<Tile, MAX_ENTITIES> Tile::list;
Array<InstanceVertex> Tile::instances;

void Tile::add(const Vec3& target_pos, const Quat& target_rot, const Vec3& offset, Transform* parent, r32 anim_time)
{
	Tile* t = list.add();
	t->relative_start_pos = target_pos + offset;
	t->relative_start_rot = target_rot * Quat::euler(PI * 0.5f, PI * 0.5f, fmod((Game::time.total + (anim_time * 2.0f)) * 5.0f, PI * 2.0f));
	t->relative_target_pos = target_pos;
	t->relative_target_rot = target_rot;
	if (parent)
	{
		parent->to_local(&t->relative_start_pos, &t->relative_start_rot);
		parent->to_local(&t->relative_target_pos, &t->relative_target_rot);
	}
	t->parent = parent;
	t->timer = 0.0f;
	t->anim_time = anim_time;
}

void Tile::draw_alpha(const RenderParams& params)
{
	if (params.update_instances())
	{
		instances.length = 0;

		const Mesh* mesh_data = Loader::mesh_instanced(Asset::Mesh::tile);
		Vec3 radius = (Vec4(mesh_data->bounds_radius, mesh_data->bounds_radius, mesh_data->bounds_radius, 0)).xyz();
		r32 f_radius = vi_max(radius.x, vi_max(radius.y, radius.z));

		for (auto i = list.iterator(); !i.is_last(); i.next())
		{
			Tile* tile = i.item();
			const r32 size = tile->scale();

			r32 blend = vi_min(tile->timer / tile->anim_time, 1.0f);
			Vec3 pos = Vec3::lerp(blend, tile->relative_start_pos, tile->relative_target_pos) + Vec3(sinf(blend * PI) * 0.25f);
			Quat rot = Quat::slerp(blend, tile->relative_start_rot, tile->relative_target_rot);
			if (tile->parent.ref())
				tile->parent.ref()->to_world(&pos, &rot);

			if (params.camera->visible_sphere(pos, size * f_radius))
			{
				InstanceVertex* instance = instances.add();
				instance->world_matrix.make_transform(pos, Vec3(size), rot);
				instance->color = Vec4(1);
			}
		}
	}

	if (instances.length == 0)
		return;

	Loader::shader_permanent(Asset::Shader::flat_instanced);

	RenderSync* sync = params.sync;
	sync->write(RenderOp::Shader);
	sync->write(Asset::Shader::flat_instanced);
	sync->write(params.technique);

	Mat4 vp = params.view_projection;

	sync->write(RenderOp::Uniform);
	sync->write(Asset::Uniform::vp);
	sync->write(RenderDataType::Mat4);
	sync->write<s32>(1);
	sync->write<Mat4>(vp);

	sync->write(RenderOp::Uniform);
	sync->write(Asset::Uniform::diffuse_color);
	sync->write(RenderDataType::Vec4);
	sync->write<s32>(1);
	sync->write<Vec4>(Vec4(1, 1, 1, 0.5f));

	sync->write(RenderOp::UpdateInstances);
	sync->write(Asset::Mesh::tile);
	sync->write(instances.length);
	sync->write<InstanceVertex>(instances.data, instances.length);

	sync->write(RenderOp::Instances);
	sync->write(Asset::Mesh::tile);
}

void Tile::clear()
{
	list.clear();
}

#define TILE_LIFE_TIME 6.0f
#define TILE_ANIM_OUT_TIME 0.3f
void Tile::update(const Update& u)
{
	timer += u.time.delta;
	if (timer > TILE_LIFE_TIME)
		list.remove(id());
}

r32 Tile::scale() const
{
	r32 blend;
	if (timer < TILE_LIFE_TIME - TILE_ANIM_OUT_TIME)
		blend = vi_min(timer / anim_time, 1.0f);
	else
		blend = Ease::quad_in(((timer - (TILE_LIFE_TIME - TILE_ANIM_OUT_TIME)) / TILE_ANIM_OUT_TIME), 1.0f, 0.0f);
	return blend * TILE_SIZE;
}

PinArray<AirWave, MAX_ENTITIES> AirWave::list;
Array<InstanceVertex> AirWave::instances;

void AirWave::add(const Vec3& pos, const Quat& rot, r32 timestamp_offset)
{
	AirWave* w = list.add();
	w->pos = pos;
	w->rot = rot;
	w->timestamp = Game::time.total + timestamp_offset;
}

void AirWave::clear()
{
	list.clear();
}

void AirWave::draw_alpha(const RenderParams& params)
{
	const r32 multiplier = Game::session.type == SessionType::Story ? 1.0f : (1.0f / 0.3f);
	const r32 lifetime = 0.3f * multiplier;
	const r32 anim_in_time = 0.02f * multiplier;
	const r32 anim_out_time = 0.15f * multiplier;
	
	if (params.update_instances())
	{
		instances.length = 0;

		const Mesh* mesh_data = Loader::mesh_instanced(Asset::Mesh::air_wave);
		Vec3 radius = (Vec4(mesh_data->bounds_radius, mesh_data->bounds_radius, mesh_data->bounds_radius, 0)).xyz();
		r32 f_radius = vi_max(radius.x, vi_max(radius.y, radius.z));

		for (auto i = list.iterator(); !i.is_last(); i.next())
		{
			AirWave* w = i.item();
			r32 timer = Game::time.total - w->timestamp;
			if (timer > 0.0f)
			{
				r32 blend = 1.0f;
				blend = vi_min(blend, Ease::cubic_out<r32>(timer / anim_in_time));
				blend = vi_min(blend, Ease::cubic_in_out<r32>(1.0f - vi_max(0.0f, vi_min(1.0f, (timer - (lifetime - anim_out_time)) / anim_out_time))));
				if (params.camera->visible_sphere(w->pos, blend * f_radius))
				{
					InstanceVertex* instance = instances.add();
					instance->world_matrix.make_transform(w->pos, Vec3(blend), w->rot);
					instance->color = Vec4(1);
				}
			}
		}
	}

	if (instances.length == 0)
		return;

	Loader::shader_permanent(Asset::Shader::flat_instanced);

	RenderSync* sync = params.sync;
	sync->write(RenderOp::Shader);
	sync->write(Asset::Shader::flat_instanced);
	sync->write(params.technique);

	Mat4 vp = params.view_projection;

	sync->write(RenderOp::Uniform);
	sync->write(Asset::Uniform::vp);
	sync->write(RenderDataType::Mat4);
	sync->write<s32>(1);
	sync->write<Mat4>(vp);

	sync->write(RenderOp::Uniform);
	sync->write(Asset::Uniform::diffuse_color);
	sync->write(RenderDataType::Vec4);
	sync->write<s32>(1);
	sync->write<Vec4>(Vec4(1, 1, 1, 0.3f));

	sync->write(RenderOp::UpdateInstances);
	sync->write(Asset::Mesh::air_wave);
	sync->write(instances.length);
	sync->write<InstanceVertex>(instances.data, instances.length);

	sync->write(RenderOp::Instances);
	sync->write(Asset::Mesh::air_wave);
}

void AirWave::update(const Update& u)
{
	if (u.time.total - timestamp > TILE_LIFE_TIME)
		list.remove(id());
}


}
