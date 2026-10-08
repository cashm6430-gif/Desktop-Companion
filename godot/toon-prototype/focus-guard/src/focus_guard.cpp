#include "focus_guard.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <chrono>
#include <thread>

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

namespace {
// Millisecond-granularity foreground restorer. The kernel force-assigns
// foreground to a newly visible window of a foreground-vacuumed desktop
// synchronously at show time -- no user-mode hook sees it (not even
// HCBT_ACTIVATE, no WM_ACTIVATE is delivered), and Godot's per-frame focus
// check notices the assignment before the node's _process can react. A
// dedicated thread shrinks the ownership window to ~2ms, far below the
// frame period. Restoring succeeds because the moment the pet owns the
// foreground its process holds SetForegroundWindow rights.
FocusGuard *g_hook_owner = nullptr;
std::atomic<bool> g_restorer_run{false};
std::thread g_restorer_thread;

void restorer_loop() {
	while (g_restorer_run.load(std::memory_order_relaxed)) {
		HWND foreground = GetForegroundWindow();
		if (foreground == nullptr) {
			// Keep the desktop out of vacuum so a window shown into it is
			// never selected for forced foreground assignment.
			HWND shell = GetShellWindow();
			if (shell != nullptr) {
				SetForegroundWindow(shell);
			}
		} else if (g_hook_owner != nullptr && foreground == (HWND)g_hook_owner->hook_instance_pet_hwnd()) {
			HWND shell = GetShellWindow();
			g_hook_owner->note_restore(shell != nullptr && SetForegroundWindow(shell) != 0);
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
}

// Thread-level CBT hook: vetoes activation requests aimed at the pet window
// (returning 1 blocks the activation). Installed on the pet window's own
// thread, no global hook. Kernel-level reassignments bypass this hook, which
// is why the restorer thread above exists.
LRESULT CALLBACK focus_guard_cbt_proc(int code, WPARAM wparam, LPARAM lparam) {
	if (code == HCBT_ACTIVATE && g_hook_owner != nullptr &&
		g_hook_owner->should_veto((void *)wparam)) {
		g_hook_owner->note_veto();
		return 1; // veto the activation
	}
	return CallNextHookEx(nullptr, code, wparam, lparam);
}

// Observer for WM_ACTIVATE deliveries the CBT hook never sees (kernel-level
// foreground reassignments). lParam names the window that LOST activation.
LRESULT CALLBACK focus_guard_cwp_proc(int code, WPARAM wparam, LPARAM lparam) {
	if (code == HC_ACTION && lparam != 0 && g_hook_owner != nullptr) {
		CWPSTRUCT *info = (CWPSTRUCT *)lparam;
		if (info->message == WM_ACTIVATE) {
			g_hook_owner->trace_activation(LOWORD(info->wParam) != WA_INACTIVE, (void *)info->lParam);
		}
	}
	return CallNextHookEx(nullptr, code, wparam, lparam);
}
} // namespace

void start_global_restorer() {
	if (g_restorer_run.exchange(true)) {
		return;
	}
	g_restorer_thread = std::thread(restorer_loop);
}

void stop_global_restorer() {
	if (!g_restorer_run.exchange(false)) {
		return;
	}
	if (g_restorer_thread.joinable()) {
		g_restorer_thread.join();
	}
}

void *FocusGuard::hook_instance_pet_hwnd() {
	return g_hook_owner ? g_hook_owner->pet_hwnd_ : nullptr;
}

bool FocusGuard::should_veto(void *candidate) {
	return veto_all_ || (candidate != nullptr && candidate == pet_hwnd_);
}

bool FocusGuard::foreign_foreground_available() {
	HWND foreground = GetForegroundWindow();
	return foreground != nullptr && (HWND)pet_hwnd_ != foreground;
}

FocusGuard::FocusGuard() {
	// Install the hooks in the constructor so they are already active when
	// the window is configured.
	install_activation_veto();
}

FocusGuard::~FocusGuard() {
	if (cbt_hook_ != nullptr) {
		UnhookWindowsHookEx((HHOOK)cbt_hook_);
		cbt_hook_ = nullptr;
	}
	if (cwp_hook_ != nullptr) {
		UnhookWindowsHookEx((HHOOK)cwp_hook_);
		cwp_hook_ = nullptr;
	}
	if (g_hook_owner == this) {
		g_hook_owner = nullptr;
	}
}

void FocusGuard::install_activation_veto() {
	if (cbt_hook_ != nullptr || g_hook_owner != nullptr) {
		return;
	}
	g_hook_owner = this;
	// The window is owned by the current (main) thread during _ready.
	cbt_hook_ = (void *)SetWindowsHookExW(WH_CBT, focus_guard_cbt_proc, nullptr, GetCurrentThreadId());
	cwp_hook_ = (void *)SetWindowsHookExW(WH_CALLWNDPROC, focus_guard_cwp_proc, nullptr, GetCurrentThreadId());
	Dictionary entry;
	entry["kind"] = "fg_sample";
	entry["at"] = "install";
	entry["fg"] = (int64_t)(intptr_t)GetForegroundWindow();
	entry["wall_time_usec"] = (int64_t)Time::get_singleton()->get_ticks_usec();
	activation_trace_.push_back(entry);
}

void FocusGuard::trace_activation(bool active, void *deactivated) {
	Dictionary entry;
	entry["kind"] = "wm_activate";
	entry["active"] = active;
	entry["deactivated"] = (int64_t)(intptr_t)deactivated;
	entry["wall_time_usec"] = (int64_t)Time::get_singleton()->get_ticks_usec();
	activation_trace_.push_back(entry);
	while (activation_trace_.size() > 8) {
		activation_trace_.pop_front();
	}
}

void FocusGuard::_bind_methods() {
	ClassDB::bind_method(D_METHOD("configure", "hwnd", "event_log_path"), &FocusGuard::configure);
	ClassDB::bind_method(D_METHOD("poll"), &FocusGuard::poll);
	ClassDB::bind_method(D_METHOD("get_stats"), &FocusGuard::get_stats);
	ClassDB::bind_method(D_METHOD("foreign_foreground_available"), &FocusGuard::foreign_foreground_available);
}

void FocusGuard::configure(int64_t hwnd, const String &event_log_path) {
	pet_hwnd_ = reinterpret_cast<void *>(static_cast<intptr_t>(hwnd));
	veto_all_ = false;
	event_log_path_ = event_log_path;
	install_activation_veto();
	{
		Dictionary entry;
		entry["kind"] = "fg_sample";
		entry["at"] = "configure";
		entry["fg"] = (int64_t)(intptr_t)GetForegroundWindow();
		entry["wall_time_usec"] = (int64_t)Time::get_singleton()->get_ticks_usec();
		activation_trace_.push_back(entry);
	}
	set_process(true);
}

void FocusGuard::_process(double delta) {
	(void)delta;
	poll();
}

void FocusGuard::poll() {
	if (pet_hwnd_ == nullptr) {
		return;
	}
	HWND foreground = GetForegroundWindow();
	if (foreground == (HWND)pet_hwnd_) {
		// Defensive fallback: the restorer thread should make this
		// unreachable, but if any path still ends with the pet as
		// foreground, give it back.
		++forced_assignments_;
		HWND target = nullptr;
		bool to_shell = false;
		for (int index = 0; index < foreign_history_count_; ++index) {
			HWND candidate = (HWND)foreign_history_[index];
			if (candidate != nullptr && IsWindow(candidate) && IsWindowVisible(candidate)) {
				target = candidate;
				break;
			}
		}
		if (target == nullptr) {
			target = GetShellWindow();
			to_shell = target != nullptr;
		}
		bool restored = target != nullptr && SetForegroundWindow(target) != 0;
		if (restored && to_shell) {
			++restores_to_shell_;
		}
		if (restored) {
			++restores_ok_;
		} else {
			++restores_failed_;
		}
		append_event(restored, target, to_shell);
		return;
	}
	if (foreground == nullptr) {
		return;
	}
	// Track recent foreign foreground windows (deduplicated, most recent first).
	if (foreign_history_count_ > 0 && foreign_history_[0] == (void *)foreground) {
		return;
	}
	for (int index = foreign_history_count_; index > 0; --index) {
		foreign_history_[index] = foreign_history_[index - 1];
	}
	foreign_history_[0] = (void *)foreground;
	if (foreign_history_count_ < kHistorySize) {
		++foreign_history_count_;
	}
}

void FocusGuard::append_event(bool restored, void *target, bool to_shell) {
	if (event_log_path_.is_empty()) {
		return;
	}
	Ref<FileAccess> file = FileAccess::open(event_log_path_, FileAccess::WRITE_READ);
	if (file.is_null()) {
		return;
	}
	file->seek_end();
	Dictionary event;
	event["event"] = "forced_foreground_assignment";
	event["engine_time_seconds"] = (double)Time::get_singleton()->get_ticks_usec() / 1000000.0;
	event["restored"] = restored;
	event["restored_to"] = (int64_t)(intptr_t)target;
	event["restored_to_shell"] = to_shell;
	file->store_line(JSON::stringify(event));
}

Dictionary FocusGuard::get_stats() const {
	Dictionary stats;
	stats["forced_assignments"] = forced_assignments_.load(std::memory_order_relaxed);
	stats["activations_vetoed"] = activations_vetoed_.load(std::memory_order_relaxed);
	stats["cbt_hook_installed"] = cbt_hook_ != nullptr;
	stats["restores_ok"] = restores_ok_.load(std::memory_order_relaxed);
	stats["restores_failed"] = restores_failed_.load(std::memory_order_relaxed);
	stats["restores_to_shell"] = restores_to_shell_.load(std::memory_order_relaxed);
	stats["configured"] = pet_hwnd_ != nullptr;
	stats["activation_trace"] = activation_trace_.duplicate(true);
	return stats;
}
