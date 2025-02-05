#include "gui.hpp"

extern "C" {
#include "ui.h"
#include "ui_comp.h"
}

void Gui::set_mute(bool muted) {
  set_muted(muted);
  if (muted) {
    lv_obj_add_state(ui_mutebutton, LV_STATE_CHECKED);
  } else {
    lv_obj_clear_state(ui_mutebutton, LV_STATE_CHECKED);
  }
}

void Gui::set_audio_level(int new_audio_level) {
  new_audio_level = std::clamp(new_audio_level, 0, 100);
  lv_bar_set_value(ui_volumebar, new_audio_level, LV_ANIM_ON);
  set_audio_volume(new_audio_level);
}

void Gui::set_video_setting(VideoSetting setting) {
  ::set_video_setting(setting);
  lv_dropdown_set_selected(ui_videosettingdropdown, (int)setting);
}

VideoSetting Gui::get_video_setting() {
  return (VideoSetting)(lv_dropdown_get_selected(ui_videosettingdropdown));
}

void Gui::add_rom(const std::string& name) {
  // protect since this function is called from another thread context
  std::lock_guard<std::recursive_mutex> lk(mutex_);
  // make a new rom, which is a button with a label in it
  // make the rom's button
  auto new_rom = lv_btn_create(ui_rompanel);
  lv_obj_set_size(new_rom, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_add_flag( new_rom, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
  lv_obj_clear_flag( new_rom, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(new_rom, &Gui::event_callback, LV_EVENT_PRESSED, static_cast<void*>(this));
  lv_obj_center(new_rom);
  // set the rom's label text
  auto label = lv_label_create(new_rom);
  lv_label_set_long_mode(label, LV_LABEL_LONG_SCROLL_CIRCULAR);
  lv_obj_set_width(label, LV_PCT(100));
  lv_obj_add_flag(label, LV_OBJ_FLAG_EVENT_BUBBLE);
  lv_obj_add_flag(label, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_label_set_text(label, name.c_str());
  lv_obj_add_style(label, &rom_label_style_, LV_STATE_DEFAULT);
  lv_obj_center(label);
  // and add it to our vector
  roms_.push_back(new_rom);
  if (focused_rom_ == -1) {
    // if we don't have a focused rom, then focus this newly added rom!
    focus_rom(new_rom);
  }
}

void Gui::focus_rom(lv_obj_t *new_focus, bool scroll_to_view) {
  std::lock_guard<std::recursive_mutex> lk(mutex_);
  if (roms_.size() == 0) {
    return;
  }
  // unfocus all roms
  for (int i=0; i < roms_.size(); i++) {
    auto rom = roms_[i];
    lv_obj_clear_state(rom, LV_STATE_CHECKED);
    if (rom == new_focus && i != focused_rom_) {
      // if the focused_rom variable was not set correctly, set it now.
      focused_rom_ = i;
    }
  }
  // focus
  lv_obj_add_state(new_focus, LV_STATE_CHECKED);

  if (scroll_to_view) {
    lv_obj_scroll_to_view(new_focus, LV_ANIM_ON);
  }

}

void Gui::deinit_ui() {
  lv_obj_del(ui_romscreen);
  lv_obj_del(ui_settingsscreen);
}

void Gui::init_ui() {
  ui_init();

  // make the label scrolling animation
  lv_anim_init(&rom_label_animation_template_);
  lv_anim_set_delay(&rom_label_animation_template_, 1000);           /*Wait 1 second to start the first scroll*/
  lv_anim_set_repeat_delay(&rom_label_animation_template_,
                           3000);    /*Repeat the scroll 3 seconds after the label scrolls back to the initial position*/

  /*Initialize the label style with the animation template*/
  lv_style_init(&rom_label_style_);
  lv_style_set_anim(&rom_label_style_, &rom_label_animation_template_);

  lv_obj_set_flex_flow(ui_rompanel, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_scroll_snap_y(ui_rompanel, LV_SCROLL_SNAP_CENTER);

  lv_bar_set_value(ui_volumebar, get_audio_volume(), LV_ANIM_OFF);

  // video settings
  lv_obj_add_event_cb(ui_videosettingdropdown, &Gui::event_callback, LV_EVENT_VALUE_CHANGED, static_cast<void*>(this));

  // volume settings
  lv_obj_add_event_cb(ui_volumeupbutton, &Gui::event_callback, LV_EVENT_PRESSED, static_cast<void*>(this));
  lv_obj_add_event_cb(ui_volumedownbutton, &Gui::event_callback, LV_EVENT_PRESSED, static_cast<void*>(this));
  lv_obj_add_event_cb(ui_mutebutton, &Gui::event_callback, LV_EVENT_PRESSED, static_cast<void*>(this));

}

void Gui::load_rom_screen() {
  lv_scr_load(ui_romscreen);
}

void Gui::on_value_changed(lv_event_t *e) {
  lv_obj_t * target = lv_event_get_target(e);
  logger_.info("Value changed: {}", fmt::ptr(target));
  // is it the settings button?
  bool is_video_setting = (target == ui_videosettingdropdown);
  if (is_video_setting) {
    set_video_setting(this->get_video_setting());
    return;
  }
}

void Gui::on_pressed(lv_event_t *e) {
  lv_obj_t * target = lv_event_get_target(e);
  logger_.info("PRESSED: {}", fmt::ptr(target));
  // volume controls
  bool is_volume_up_button = (target == ui_volumeupbutton);
  if (is_volume_up_button) {
    set_audio_level(get_audio_volume() + 10);
    return;
  }
  bool is_volume_down_button = (target == ui_volumedownbutton);
  if (is_volume_down_button) {
    set_audio_level(get_audio_volume() - 10);
    return;
  }
  bool is_mute_button = (target == ui_mutebutton);
  if (is_mute_button) {
    toggle_mute();
    return;
  }
  // or is it one of the roms?
  if (std::find(roms_.begin(), roms_.end(), target) != roms_.end()) {
    // it's one of the roms, focus it! this was pressed, so don't scroll (it
    // will already scroll)
    focus_rom(target, false);
  }
}
