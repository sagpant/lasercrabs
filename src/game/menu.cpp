#include "menu.h"
#include "asset/level.h"
#include "asset/font.h"
#include "asset/mesh.h"
#include "game.h"
#include "player.h"
#include "render/ui.h"
#include "load.h"
#include "entities.h"
#include "audio.h"
#include "asset/Wwise_IDs.h"
#include "render/views.h"
#include "console.h"
#include "mersenne/mersenne-twister.h"
#include "strings.h"

namespace VI
{

namespace Menu
{

#define fov_initial (80.0f * PI * 0.5f / 180.0f)

#if DEBUG
#define CONNECT_DELAY_MIN 0.5f
#define CONNECT_DELAY_RANGE 0.0f
#else
#define CONNECT_DELAY_MIN 2.0f
#define CONNECT_DELAY_RANGE 2.0f
#endif

static Camera* cameras[MAX_GAMEPADS] = {};
static UIText player_text[MAX_GAMEPADS];
static s32 gamepad_count = 0;
static AssetID last_level = AssetNull;
static AssetID next_level = AssetNull;
static Game::Mode next_mode = Game::Mode::Pvp;
static r32 connect_timer = 0.0f;
static UIMenu main_menu;

static State main_menu_state;

void reset_players()
{
	AssetID level = Game::data.level;
	Game::data = Game::Data();
	Game::data.level = level;
	for (int i = 0; i < MAX_GAMEPADS; i++)
		Game::data.local_player_config[i] = AI::Team::None;
	Game::data.local_player_config[0] = AI::Team::A;
}

void init()
{
	Loader::font_permanent(Asset::Font::lowpoly);
	refresh_variables();

	for (s32 i = 0; i < MAX_GAMEPADS; i++)
	{
		player_text[i].font = Asset::Font::lowpoly;
		player_text[i].size = 18.0f;
		player_text[i].anchor_x = UIText::Anchor::Center;
		player_text[i].anchor_y = UIText::Anchor::Min;
		char buffer[255];
		sprintf(buffer, _(strings::player), i + 1);
		player_text[i].text(buffer);
	}

	reset_players();

	title();
}

void clear()
{
	for (s32 i = 0; i < MAX_GAMEPADS; i++)
	{
		if (cameras[i])
		{
			cameras[i]->remove();
			cameras[i] = nullptr;
		}
	}
	main_menu_state = State::Hidden;
}

void refresh_variables()
{
	b8 gamepad = gamepad_count > 0;
	UIText::set_variable("Start", Game::bindings.start.string(gamepad));
	UIText::set_variable("Resume", Game::bindings.pause.string(gamepad));
	UIText::set_variable("Action", Game::bindings.action.string(gamepad));
	UIText::set_variable("Cancel", Game::bindings.cancel.string(gamepad));

	const Settings& settings = Loader::settings();
	const Settings::Bindings& bindings = settings.bindings;
	UIText::set_variable("Primary", bindings.primary.string(gamepad));
	UIText::set_variable("Secondary", bindings.secondary.string(gamepad));
	if (gamepad)
		UIText::set_variable("Movement", _(strings::left_joystick));
	else
	{
		char buffer[512];
		sprintf(buffer, _(strings::keyboard_movement), bindings.forward.string(gamepad), bindings.left.string(gamepad), bindings.backward.string(gamepad), bindings.right.string(gamepad));
		UIText::set_variable("Movement", buffer);
	}
	UIText::set_variable("Parkour", bindings.parkour.string(gamepad));
	UIText::set_variable("Jump", bindings.jump.string(gamepad));
	UIText::set_variable("Slide", bindings.slide.string(gamepad));
	UIText::set_variable("Menu", bindings.menu.string(gamepad));
}

void title_menu(const Update& u, u8 gamepad, UIMenu* menu, State* state)
{
	if (*state == State::Hidden)
		*state = State::Visible;
	menu->start(u, 0);
	switch (*state)
	{
		case State::Visible:
		{
			Vec2 pos(u.input->width * 0.5f, u.input->height * 0.5f + UIMenu::height(5) * 0.5f);
			if (menu->item(u, &pos, _(strings::continue_)))
			{
				transition(Asset::Level::start, Game::Mode::Special);
				return;
			}
			menu->item(u, &pos, _(strings::new_));
			if (menu->item(u, &pos, _(strings::options)))
			{
				menu->selected = 0;
				*state = State::Options;
			}
			if (menu->item(u, &pos, _(strings::splitscreen)))
				splitscreen();
			if (menu->item(u, &pos, _(strings::exit)))
				Game::quit = true;
			break;
		}
		case State::Options:
		{
			Vec2 pos(u.input->width * 0.5f, u.input->height * 0.5f + options_height() * 0.5f);
			if (!options(u, 0, menu, &pos))
			{
				menu->selected = 0;
				*state = State::Visible;
			}
			break;
		}
		default:
		{
			vi_assert(false);
			break;
		}
	}
	menu->end();
}

void pause_menu(const Update& u, const Rect2& viewport, u8 gamepad, UIMenu* menu, State* state)
{
	if (*state == State::Hidden)
	{
		menu->clear();
		return;
	}

	menu->start(u, 0);
	switch (*state)
	{
		case State::Visible:
		{
			Vec2 pos(0, viewport.size.y * 0.5f + UIMenu::height(5) * 0.5f);
			if (menu->item(u, &pos, _(strings::resume)))
				*state = State::Hidden;
			if (menu->item(u, &pos, _(strings::options)))
			{
				menu->selected = 0;
				*state = State::Options;
			}
			if (menu->item(u, &pos, _(strings::main_menu)))
				Menu::title();
			break;
		}
		case State::Options:
		{
			Vec2 pos(0, viewport.size.y * 0.5f + options_height() * 0.5f);
			if (!options(u, 0, menu, &pos))
			{
				menu->selected = 0;
				*state = State::Visible;
			}
			break;
		}
		default:
		{
			vi_assert(false);
			break;
		}
	}
	menu->end();
}

void update(const Update& u)
{
	s32 last_gamepad_count = gamepad_count;
	gamepad_count = 0;
	for (s32 i = 0; i < MAX_GAMEPADS; i++)
	{
		if (u.input->gamepads[i].active)
			gamepad_count++;
	}

	if ((last_gamepad_count == 0) != (gamepad_count != 0))
		refresh_variables();

	if (Console::visible)
		return;

	switch (Game::data.level)
	{
		case Asset::Level::splitscreen:
		{
			if (Game::data.level != last_level)
			{
				reset_players();
				Game::data.local_multiplayer = true;
			}

			b8 cameras_changed = false;
			s32 screen_count = 0;
			for (s32 i = 0; i < MAX_GAMEPADS; i++)
			{
				b8 screen_active = u.input->gamepads[i].active || i == 0;

				if (screen_active && !cameras[i])
				{
					cameras[i] = Camera::add();
					cameras_changed = true;
				}
				else if (cameras[i] && !screen_active)
				{
					cameras[i]->remove();
					cameras[i] = nullptr;
					cameras_changed = true;
				}

				if (screen_active)
				{
					screen_count++;
					if (i > 0) // player 0 must stay in
					{
						if (u.input->get(Game::bindings.cancel, i) && !u.last_input->get(Game::bindings.cancel, i))
						{
							Game::data.local_player_config[i] = AI::Team::None;
							Audio::post_global_event(AK::EVENTS::PLAY_BEEP_GOOD);
						}
						else if (u.input->get(Game::bindings.action, i) && !u.last_input->get(Game::bindings.action, i))
						{
							Game::data.local_player_config[i] = i % 2 == 0 ? AI::Team::A : AI::Team::B;
							Audio::post_global_event(AK::EVENTS::PLAY_BEEP_GOOD);
						}
					}
				}
				else
					Game::data.local_player_config[i] = AI::Team::None;
			}

			if (cameras_changed)
			{
				Camera::ViewportBlueprint* viewports = Camera::viewport_blueprints[screen_count - 1];
				for (s32 i = 0; i < MAX_GAMEPADS; i++)
				{
					Camera* camera = cameras[i];
					if (camera)
					{
						Camera::ViewportBlueprint* viewport = &viewports[i];
						camera->viewport =
						{
							Vec2((s32)(viewport->x * (r32)u.input->width), (s32)(viewport->y * (r32)u.input->height)),
							Vec2((s32)(viewport->w * (r32)u.input->width), (s32)(viewport->h * (r32)u.input->height)),
						};
						r32 aspect = camera->viewport.size.y == 0 ? 1 : (r32)camera->viewport.size.x / (r32)camera->viewport.size.y;
						camera->perspective(fov_initial, aspect, 0.01f, Skybox::far_plane);
					}
				}
			}

			b8 start = false;
			for (s32 i = 0; i < MAX_GAMEPADS; i++)
			{
				if (Game::data.local_player_config[i] != AI::Team::None)
					start |= u.input->get(Game::bindings.start, i) && !u.last_input->get(Game::bindings.start, i);
			}

			if (Game::data.level != last_level && gamepad_count <= 1)
				start = true;

			if (start)
				transition(Asset::Level::pvp0, Game::Mode::Pvp);
			break;
		}
		case Asset::Level::title:
		{
			if (Game::data.level != last_level)
			{
				reset_players();
				main_menu_state = State::Visible;
			}
			break;
		}
		case Asset::Level::connect:
		{
			if (Game::data.level != last_level)
				connect_timer = CONNECT_DELAY_MIN + mersenne::randf_co() * CONNECT_DELAY_RANGE;

			connect_timer -= u.time.delta;
			if (connect_timer < 0.0f)
			{
				clear();
				Game::schedule_load_level(next_level, next_mode);
				next_level = AssetNull;
			}
			break;
		}
		case AssetNull:
			break;
		default: // just playing normally
			break;
	}

	if (Game::data.level == Asset::Level::title)
		title_menu(u, 0, &main_menu, &main_menu_state);
	else if (is_special_level(Game::data.level, Game::data.mode))
	{
		// toggle the pause menu
		b8 pause_hit;
		if (Game::data.level == Asset::Level::splitscreen)
			pause_hit = u.input->get(Game::bindings.cancel, 0) && !u.last_input->get(Game::bindings.cancel, 0);
		else
			pause_hit = u.input->get(Game::bindings.pause, 0) && !u.last_input->get(Game::bindings.pause, 0);

		if (pause_hit && Game::time.total > 0.0f && (main_menu_state == State::Hidden || main_menu_state == State::Visible))
			main_menu_state = main_menu_state == State::Hidden ? State::Visible : State::Hidden;

		// do pause menu
		const Rect2& viewport = cameras[0] ? cameras[0]->viewport : Rect2(Vec2(0, 0), Vec2(u.input->width, u.input->height));
		pause_menu(u, viewport, 0, &main_menu, &main_menu_state);
	}

	last_level = Game::data.level;
}

void transition(AssetID level, Game::Mode mode)
{
	clear();
	if (mode == Game::Mode::Pvp)
	{
		next_level = level;
		next_mode = mode;
		Game::schedule_load_level(Asset::Level::connect, Game::Mode::Special);
	}
	else
		Game::schedule_load_level(level, mode);
}

void splitscreen()
{
	clear();
	Game::schedule_load_level(Asset::Level::splitscreen, Game::Mode::Special);
}

void title()
{
	clear();
	Game::schedule_load_level(Asset::Level::title, Game::Mode::Special);
}

void draw(const RenderParams& params)
{
	if (params.technique != RenderTechnique::Default)
		return;

	const Rect2& viewport = params.camera->viewport;
	switch (Game::data.level)
	{
		case Asset::Level::splitscreen:
		{
			UIText text;
			text.anchor_x = UIText::Anchor::Center;
			text.anchor_y = UIText::Anchor::Max;
			const Vec2 box_size(256 * UI::scale);
			for (s32 i = 0; i < MAX_GAMEPADS; i++)
			{
				if (params.camera == cameras[i])
				{
					UI::box(params, { viewport.size * 0.5f - box_size * 0.5f, box_size }, UI::background_color);
					player_text[i].draw(params, viewport.size * 0.5f);
					if (i > 0)
					{
						if (Game::data.local_player_config[i] == AI::Team::None)
							text.text(_(strings::join));
						else
							text.text(_(strings::leave));
					}
					else // player 0 must stay in
						text.text(_(strings::begin));
					text.draw(params, viewport.size * 0.5f + Vec2(0, -16.0f * UI::scale));
					break;
				}
			}
			break;
		}
		case Asset::Level::title:
		{
			Vec2 logo_pos = viewport.size * Vec2(0.35f, 0.5f);
			Vec2 logo_size(128.0f * UI::scale);
			UI::box(params, { Vec2(0, viewport.size.y * 0.5f + logo_size.y * -1.0f), Vec2(viewport.size.x, logo_size.y * 2.0f) }, UI::background_color);
			Mesh* m0 = Loader::mesh(Asset::Mesh::logo_mesh);
			UI::mesh(params, Asset::Mesh::logo_mesh, logo_pos, logo_size, m0->color);
			Mesh* m1 = Loader::mesh(Asset::Mesh::logo_mesh_1);
			UI::mesh(params, Asset::Mesh::logo_mesh_1, logo_pos, logo_size, m1->color);
			break;
		}
		case Asset::Level::connect:
		{
			UIText text;
			text.anchor_x = text.anchor_y = UIText::Anchor::Center;
			text.text(_(strings::connecting));
			Vec2 pos = viewport.size * 0.5f;

			UI::box(params, text.rect(pos).pad({ Vec2(64, 24) * UI::scale, Vec2(18, 24) * UI::scale }), UI::background_color);

			text.draw(params, pos);

			Vec2 triangle_pos = Vec2
			(
				pos.x - text.bounds().x * 0.5f - 32.0f * UI::scale,
				pos.y
			);
			UI::triangle_border(params, { triangle_pos, Vec2(20 * UI::scale) }, 6, UI::default_color, Game::time.total * 8.0f);
			break;
		}
		default:
			break;
	}

	if (is_special_level(Game::data.level, Game::data.mode))
	{
		if (!cameras[0] || params.camera == cameras[0]) // if we have cameras active, only draw the main menu on the first one
			main_menu.draw_alpha(params);
	}
}

b8 is_special_level(AssetID level, Game::Mode mode)
{
	return mode == Game::Mode::Special
		|| level == Asset::Level::connect
		|| level == Asset::Level::title
		|| level == Asset::Level::splitscreen;
}

b8 options(const Update& u, u8 gamepad, UIMenu* menu, Vec2* pos)
{
	bool menu_open = true;
	if (menu->item(u, pos, _(strings::back)) || (!u.input->get(Game::bindings.cancel, gamepad) && u.last_input->get(Game::bindings.cancel, gamepad)))
		menu_open = false;

	Settings& settings = Loader::settings();
	char str[128];
	UIMenu::Delta delta;

	sprintf(str, "%d", settings.sfx);
	delta = menu->slider_item(u, pos, _(strings::sfx), str);
	if (delta == UIMenu::Delta::Down)
		settings.sfx = vi_max(0, settings.sfx - 10);
	else if (delta == UIMenu::Delta::Up)
		settings.sfx = vi_min(100, settings.sfx + 10);
	if (delta != UIMenu::Delta::None)
		Audio::global_param(AK::GAME_PARAMETERS::SFXVOL, (r32)settings.sfx / 100.0f);

	sprintf(str, "%d", settings.music);
	delta = menu->slider_item(u, pos, _(strings::music), str);
	if (delta == UIMenu::Delta::Down)
		settings.music = vi_max(0, settings.music - 10);
	else if (delta == UIMenu::Delta::Up)
		settings.music = vi_min(100, settings.music + 10);
	if (delta != UIMenu::Delta::None)
		Audio::global_param(AK::GAME_PARAMETERS::MUSICVOL, (r32)settings.music / 100.0f);

	if (!menu_open)
		Loader::settings_save();

	return menu_open;
}

r32 options_height()
{
	return UIMenu::height(3);
}

}

Rect2 UIMenu::Item::rect() const
{
	Vec2 bounds = label.bounds();
	Rect2 box;
	box.pos.x = pos.x - MENU_ITEM_PADDING_LEFT;
	box.pos.y = pos.y - bounds.y - MENU_ITEM_PADDING;
	box.size.x = MENU_ITEM_WIDTH;
	box.size.y = bounds.y + MENU_ITEM_PADDING * 2.0f;
	return box;
}

Rect2 UIMenu::Item::down_rect() const
{
	Rect2 r = rect();
	r.pos.x += MENU_ITEM_PADDING_LEFT + MENU_ITEM_WIDTH * 0.5f;
	r.size.x = r.size.y;
	return r;
}

Rect2 UIMenu::Item::up_rect() const
{
	Rect2 r = rect();
	r32 width = r.size.x;
	r.size.x = r.size.y;
	r.pos.x += width - r.size.x;
	return r;
}

UIMenu::UIMenu()
	: selected(),
	items()
{
}

void UIMenu::clear()
{
	items.length = 0;
}

#define JOYSTICK_DEAD_ZONE 0.5f

void UIMenu::start(const Update& u, u8 g)
{
	clear();
	gamepad = g;

	if (gamepad == 0)
		Game::update_cursor(u);

	if (u.input->gamepads[gamepad].active)
	{
		r32 last_y = Input::dead_zone(u.last_input->gamepads[gamepad].left_y, JOYSTICK_DEAD_ZONE);
		if (last_y == 0.0f)
		{
			r32 y = Input::dead_zone(u.input->gamepads[gamepad].left_y, JOYSTICK_DEAD_ZONE);
			if (y < 0.0f)
				selected--;
			else if (y > 0.0f)
				selected++;
		}
	}

	const Settings& settings = Loader::settings();
	if (u.input->get(settings.bindings.forward, gamepad)
		&& !u.last_input->get(settings.bindings.forward, gamepad))
		selected--;

	if (u.input->get(settings.bindings.backward, gamepad)
		&& !u.last_input->get(settings.bindings.backward, gamepad))
		selected++;
}

Rect2 UIMenu::add_item(Vec2* pos, b8 slider, const char* string, const char* value, b8 disabled, AssetID icon)
{
	Item* item = items.add();
	item->icon = icon;
	item->slider = slider;
	item->label.size = item->value.size = MENU_ITEM_FONT_SIZE;
	if (value)
		item->label.wrap_width = MENU_ITEM_VALUE_OFFSET - MENU_ITEM_PADDING - MENU_ITEM_PADDING_LEFT;
	else
		item->label.wrap_width = MENU_ITEM_WIDTH - MENU_ITEM_PADDING - MENU_ITEM_PADDING_LEFT;
	item->label.anchor_x = UIText::Anchor::Min;
	item->label.anchor_y = item->value.anchor_y = UIText::Anchor::Max;
	item->label.color = item->value.color = disabled ? UI::disabled_color : UI::default_color;
	item->label.text(string);

	item->value.anchor_x = UIText::Anchor::Center;
	item->value.text(value);

	item->pos = *pos;
	item->pos.x += MENU_ITEM_PADDING_LEFT;

	Rect2 box = item->rect();

	pos->y -= box.size.y;

	return box;
}

// render a single menu item and increment the position for the next item
b8 UIMenu::item(const Update& u, Vec2* menu_pos, const char* string, const char* value, b8 disabled, AssetID icon)
{
	Rect2 box = add_item(menu_pos, false, string, value, disabled, icon);

	if (gamepad == 0 && box.contains(Game::cursor))
	{
		selected = items.length - 1;

		if (disabled)
			return false;

		if (!u.input->get({ KeyCode::MouseLeft }, gamepad)
			&& u.last_input->get({ KeyCode::MouseLeft }, gamepad)
			&& Game::time.total > 0.5f)
		{
			Audio::post_global_event(AK::EVENTS::PLAY_BEEP_GOOD);
			return true;
		}
	}

	if (selected == items.length - 1
		&& !u.input->get(Game::bindings.action, gamepad)
		&& u.last_input->get(Game::bindings.action, gamepad)
		&& Game::time.total > 0.5f
		&& !Console::visible)
	{
		Audio::post_global_event(AK::EVENTS::PLAY_BEEP_GOOD);
		return true;
	}
	
	return false;
}

UIMenu::Delta UIMenu::slider_item(const Update& u, Vec2* menu_pos, const char* label, const char* value, b8 disabled, AssetID icon)
{
	Rect2 box = add_item(menu_pos, true, label, value, disabled, icon);

	if (gamepad == 0 && box.contains(Game::cursor))
		selected = items.length - 1;

	if (disabled)
		return Delta::None;

	if (selected == items.length - 1
		&& Game::time.total > 0.5f)
	{
		const Settings& settings = Loader::settings();
		if (!u.input->get(settings.bindings.left, gamepad)
			&& u.last_input->get(settings.bindings.left, gamepad))
		{
			Audio::post_global_event(AK::EVENTS::PLAY_BEEP_GOOD);
			return Delta::Down;
		}

		if (!u.input->get(settings.bindings.right, gamepad)
			&& u.last_input->get(settings.bindings.right, gamepad))
		{
			Audio::post_global_event(AK::EVENTS::PLAY_BEEP_GOOD);
			return Delta::Up;
		}

		if (u.input->gamepads[gamepad].active)
		{
			r32 last_x = Input::dead_zone(u.last_input->gamepads[gamepad].left_x, JOYSTICK_DEAD_ZONE);
			if (last_x == 0.0f)
			{
				r32 x = Input::dead_zone(u.input->gamepads[gamepad].left_x, JOYSTICK_DEAD_ZONE);
				if (x < 0.0f)
				{
					Audio::post_global_event(AK::EVENTS::PLAY_BEEP_GOOD);
					return Delta::Down;
				}
				else if (x > 0.0f)
				{
					Audio::post_global_event(AK::EVENTS::PLAY_BEEP_GOOD);
					return Delta::Up;
				}
			}
		}

		if (gamepad == 0)
		{
			if (!u.input->get({ KeyCode::MouseLeft }, gamepad)
				&& u.last_input->get({ KeyCode::MouseLeft }, gamepad)
				&& Game::time.total > 0.5f)
			{
				Item* item = &items[items.length - 1];
				if (item->down_rect().contains(Game::cursor))
				{
					Audio::post_global_event(AK::EVENTS::PLAY_BEEP_GOOD);
					return Delta::Down;
				}

				if (item->up_rect().contains(Game::cursor))
				{
					Audio::post_global_event(AK::EVENTS::PLAY_BEEP_GOOD);
					return Delta::Up;
				}
			}
		}
	}
	
	return Delta::None;
}

void UIMenu::end()
{
	if (selected < 0)
		selected = items.length - 1;
	if (selected >= items.length)
		selected = 0;
}

r32 UIMenu::height(s32 items)
{
	return (items * MENU_ITEM_HEIGHT) - MENU_ITEM_PADDING * 2.0f;
}

void UIMenu::draw_alpha(const RenderParams& params) const
{
	if (items.length == 0)
		return;

	for (s32 i = 0; i < items.length; i++)
	{
		const Item* item = &items[i];
		UI::box(params, item->rect(), i == selected ? UI::subtle_color : UI::background_color);

		if (item->icon != AssetNull)
			UI::mesh(params, item->icon, item->pos + Vec2(MENU_ITEM_PADDING_LEFT * -0.5f, MENU_ITEM_FONT_SIZE * -0.5f), Vec2(UI::scale * MENU_ITEM_FONT_SIZE), item->label.color);

		item->label.draw(params, item->pos);
		if (item->value.has_text())
			item->value.draw(params, item->pos + Vec2(MENU_ITEM_VALUE_OFFSET, 0));
		if (item->slider)
		{
			const Rect2& down_rect = item->down_rect();
			UI::box(params, down_rect, UI::background_color);
			UI::triangle(params, { down_rect.pos + down_rect.size * 0.5f, down_rect.size * 0.5f }, item->label.color, PI * 0.5f);

			const Rect2& up_rect = item->up_rect();
			UI::box(params, up_rect, UI::background_color);
			UI::triangle(params, { up_rect.pos + up_rect.size * 0.5f, up_rect.size * 0.5f }, item->label.color, PI * -0.5f);
		}
	}

	if (gamepad == 0)
		Game::draw_cursor(params);
}

}
