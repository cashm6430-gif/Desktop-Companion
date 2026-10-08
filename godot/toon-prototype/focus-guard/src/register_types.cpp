#include "register_types.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/godot.hpp>

#include "focus_guard.h"

using namespace godot;

void initialize_focus_guard(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
	GDREGISTER_CLASS(FocusGuard);
	// The restorer thread must be alive BEFORE the main window is shown:
	// the kernel assigns foreground to a window shown into a desktop
	// foreground vacuum, and only a sub-frame-granularity poll can return
	// it before Godot's own per-frame focus check records focus_entered.
	// Not for the editor (its windows must never be touched).
	if (!Engine::get_singleton()->is_editor_hint()) {
		start_global_restorer();
	}
}

void uninitialize_focus_guard(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
	stop_global_restorer();
}

extern "C" GDExtensionBool GDE_EXPORT focus_guard_library_init(
		GDExtensionInterfaceGetProcAddress p_get_proc, const GDExtensionClassLibraryPtr p_library,
		GDExtensionInitialization *r_initialization) {
	GDExtensionBinding::InitObject init_obj(p_get_proc, p_library, r_initialization);
	init_obj.register_initializer(initialize_focus_guard);
	init_obj.register_terminator(uninitialize_focus_guard);
	init_obj.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);
	return init_obj.init();
}
