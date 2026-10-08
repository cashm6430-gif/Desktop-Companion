#ifndef FOCUS_GUARD_H
#define FOCUS_GUARD_H

#include <atomic>

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/variant/array.hpp>

// Keeps the desktop pet from keeping a system-forced foreground assignment.
//
// The pet window is TOPMOST and WS_EX_NOACTIVATE, but Windows still hands it
// the foreground whenever the desktop has a foreground vacuum (the active
// window was destroyed). Three layers of defense:
//   1. a thread-level CBT hook vetoes ordinary activation requests;
//   2. a 2ms restorer thread returns any kernel-level forced assignment
//      (which bypasses all user-mode hooks) before Godot's per-frame focus
//      check can observe it;
//   3. the node's per-frame poll keeps evidence and restores anything left.
// Every forced assignment is appended to a JSONL evidence file when a review
// output path is configured.
class FocusGuard : public godot::Node {
	GDCLASS(FocusGuard, godot::Node)

private:
	static constexpr int kHistorySize = 8;

	void *pet_hwnd_ = nullptr;
	void *foreign_history_[kHistorySize] = {};
	int foreign_history_count_ = 0;
	std::atomic<int> forced_assignments_{0};
	std::atomic<int> restores_ok_{0};
	std::atomic<int> restores_failed_{0};
	std::atomic<int> restores_to_shell_{0};
	std::atomic<int> activations_vetoed_{0};
	void *cbt_hook_ = nullptr;
	void *cwp_hook_ = nullptr;
	// Until configure() provides the pet hwnd, veto every activation on
	// this thread: the startup foreground vacuum can hit between window
	// creation and configuration, and the pet must never own foreground.
	bool veto_all_ = true;
	godot::String event_log_path_;
	// Diagnostic ring of observed WM_ACTIVATE messages and foreground
	// samples (kernel-level activations never reach the CBT hook).
	godot::Array activation_trace_;

	void append_event(bool restored, void *target, bool to_shell);
	void install_activation_veto();

protected:
	static void _bind_methods();

public:
	FocusGuard();
	// Read by the thread-level CBT hook (same process, main thread).
	static void *hook_instance_pet_hwnd();
	// Hook-side decision: veto-all before configure(), pet-only afterwards.
	bool should_veto(void *candidate);
	// Called from the WH_CALLWNDPROC observer hook.
	void trace_activation(bool active, void *deactivated);
	// True while some foreign window owns the foreground: safe to be visible.
	bool foreign_foreground_available();
	// Unhook the CBT hook BEFORE the engine tears down, otherwise the
	// process crashes with STATUS_FATAL_USER_CALLBACK_EXCEPTION at exit.
	~FocusGuard() override;
	void configure(int64_t hwnd, const godot::String &event_log_path);
	void poll();
	void note_veto() { activations_vetoed_.fetch_add(1, std::memory_order_relaxed); }
	// Called from the restorer thread.
	void note_restore(bool ok) {
		(ok ? restores_ok_ : restores_failed_).fetch_add(1, std::memory_order_relaxed);
	}
	godot::Dictionary get_stats() const;
	void _process(double delta) override;
};

// Global 2ms restorer thread; started at SCENE init for non-editor runs.
void start_global_restorer();
void stop_global_restorer();

#endif // FOCUS_GUARD_H
