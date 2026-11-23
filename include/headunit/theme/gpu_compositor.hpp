// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <headunit/theme/scene.hpp>
#include <headunit/theme/native_frame.hpp>
#include <headunit/theme/presentation_clock.hpp>
#include <span>
#include <functional>
#include <headunit/touch_input.hpp>

namespace headunit::theme::graphics {
struct LocalPresentationDiagnostics;
struct GpuQuad {
  Rect rect;
  Color color;
};
// A scene drawing primitive, expressed in layer-local coordinates. to_output
// already includes the layer/ancestor transforms. Colours interpolate in
// premultiplied space, like the CPU reference's conical gradients.
struct GpuConicalGradient {
  Rect bounds;
  Transform to_output;
  Point start{.5, 0}, end{.5, 1};
  std::vector<Stop> stops;
  double opacity = 1, corner_radius = 0;
  std::shared_ptr<const AlphaMask> mask;  //Output pixels; null means unmasked.
};
struct GpuMaskStats {
  std::uint64_t uploads = 0, uploaded_bytes = 0, resident_bytes = 0, entries = 0;
};
struct GpuTimings {
  std::uint64_t import_ns = 0, draw_ns = 0;
  std::uint64_t validation_ns = 0, completion_wait_ns = 0, overlap_ns = 0;
};
struct GpuSceneCacheStats {
  std::uint64_t hits = 0, builds = 0, evictions = 0, bytes = 0, entries = 0;
  std::uint64_t image_hits = 0, image_builds = 0, image_bytes = 0, gpu_cache_bytes = 0;
  std::uint64_t backdrop_output_clips = 0;
  std::uint64_t backdrop_mask_replacement_clips = 0;
  std::uint64_t backdrop_group_regions = 0, backdrop_group_fusions = 0, backdrop_mask_shaders = 0;
  std::uint64_t backdrop_snapshot_regions = 0, backdrop_snapshot_saved_pixels = 0;
  std::uint64_t simple_mask_groups = 0;
  std::uint64_t stroke_builds = 0, stroke_hits = 0, stroke_raster_ns = 0, group_saves = 0;
  std::uint64_t additive_mask_fusions = 0;
  std::uint64_t simple_fill_mask_groups = 0;
  std::uint64_t preparation_ns = 0, traversal_ns = 0, submit_ns = 0;
  std::uint64_t deferred_gpu_frames = 0, max_deferred_gpu_frames = 0;
};
struct WaylandOptions {
  std::string title = "Headunit native video";
  // Explicit desktop preview: render on this GPU, then copy the final image
  // to wl_shm. The visible compositor supplies the presentation receipt.
  // Empty keeps direct EGL/DMA-buf output with no per-frame CPU readback.
  std::string readback_render_node{};
  // Empty uses WAYLAND_DISPLAY. Separate sockets allow bench panels to have
  // independent compositor outputs instead of hiding one another in kiosk mode.
  std::string display_name{};
  // Standard wl_output name preferred by xdg_toplevel.set_fullscreen.
  std::string output_name{};
  std::string app_id = "org.headunit.ultra.display";
  bool background_layer = false;   // Local phone-free underlay; never accepts input.
  bool accept_touch = false;       // Main panel only, in logical scene coordinates.
  bool touch_diagnostics = false;  // At most 16 physical-down coordinate samples.
  // Composite the final window onto black and declare its matching opaque
  // region. Keeps the scene/reference alpha unchanged; may permit scanout.
  bool opaque_background = false;
  // Experimental direct-EGL path. Retain native texture leases until the GPU
  // fence signals, but do not block scene submission on that fence. Its bounded
  // GPU queue uses max_pending_presentations (at most two), with a separate
  // immutable image/import lease and completion deadline for each submission.
  bool defer_scene_fence = false;
  // Experimental maximum outstanding Wayland presentations. Default preserves
  // the single-frame receipt gate used by the live receiver.
  unsigned max_pending_presentations = 1;
  // Borrowed only by the signed-local background owner; no phone-path observer.
  LocalPresentationDiagnostics* local_diagnostics = nullptr;
};
using PresentationReceipt = video::PresentationFeedback;
// Underlay submissions clear the complete final window, including fitted bars,
// and remove its opaque region in the same buffer commit. Receipts remain real.
enum class WindowContent { rendered_scene, reveal_underlay };
// Offscreen GPU composition, on one owner thread. Native buffers remain leased
// until a GPU completion fence. This is not a Wayland presentation receipt.
class GpuCompositor {
 public:
  GpuCompositor(std::uint32_t width, std::uint32_t height, const std::string& render_node);
  GpuCompositor(std::uint32_t width, std::uint32_t height, WaylandOptions);
  ~GpuCompositor();
  GpuCompositor(const GpuCompositor&) = delete;
  GpuCompositor& operator=(const GpuCompositor&) = delete;
  GpuTimings compose(std::shared_ptr<const video::NativeFrame>,
                     std::span<const GpuQuad> overlays = {});
  // Explicit CPU-reference output for full-scene integration diagnostics.
  // This uploads the already-rasterized premultiplied image; it is never an
  // automatic fallback from native video import or GPU scene composition.
  GpuTimings compose_reference(const Image&);
  // GPU-only conical paint operations, in order, over a premultiplied clear.
  // This is a primitive for the scene renderer, not a complete scene traversal.
  // Optional immutable alpha masks are uploaded once and cached. No full-scene
  // image rasterization/upload or readback is used by this path.
  GpuTimings compose_conical(std::span<const GpuConicalGradient>, Color clear = {});
  GpuMaskStats mask_stats() const;
#ifdef HEADUNIT_THEME_HAVE_SKIA
  // Full scene traversal via the optional pinned Ganesh adapter. Unsupported
  // features are rejected before the framebuffer changes; no CPU fallback.
  // The caller must keep the scene and its images stable throughout this
  // synchronous call. Mutable image pixels are compared once per frame.
  // Explicit offline probes may measure cold shader startup with a longer
  // fence wait (max2s). The default/live completion deadline stays100ms.
  // Optional owner-thread CPU work after scene commands are flushed and the
  // fence is inserted, before waiting for GPU completion. The work must not
  // issue GL calls or access this compositor. Its time counts against the same
  // absolute completion deadline; imported frames remain leased throughout.
  GpuTimings compose_scene(const std::shared_ptr<const Layer>&,
                           std::uint64_t completion_timeout_ns = 100000000,
                           const std::function<void()>& during_completion = {});
  GpuSceneCacheStats scene_cache_stats() const;
#endif
  // One outstanding presentation by default, with an opt-in bound of two.
  // submit() performs an EGL swap
  // with interval zero; it does not wait for presentation feedback. The driver
  // may still spend time submitting GPU work. poll_presentation() never waits
  // for socket readiness; call it regularly (including while a frame is pending).
  // It returns each actual presented/discarded receipt once. Missing feedback
  // faults the compositor two seconds after submission. Neither operation
  // advances a stream or script PTS; the owner applies the receipt explicitly.
  void submit(std::uint64_t token, Image* verification = nullptr,
              WindowContent content = WindowContent::rendered_scene);
  std::vector<headunit::TouchInput> take_touches(bool enabled);
  bool presentation_pending() const;
  bool presentation_slot_available() const;
  // Change the presentation (and deferred-fence scene) bound to 1 or 2 on the
  // owner thread. Pending presentations stay pending; extra deferred scenes
  // finish before the next composition.
  void set_presentation_queue_depth(unsigned depth);
  // Desktop previews retain one unresolved update while hidden. The owner
  // must release staged source frames and suspend timestamp reporting while
  // this is true; direct output still faults on its normal deadline.
  bool preview_feedback_delayed() const;
  std::optional<PresentationReceipt> poll_presentation();
  // Submit the completed frame, then wait at most two seconds for actual
  // wp_presentation feedback. A frame callback or EGL swap is not a receipt.
  // This bounded blocking API is for integration probes, not a display scheduler.
  // Optional readback verifies the final window blit before swapping; never
  // requested by normal presentation. It does not capture physical scanout.
  PresentationReceipt present(std::uint64_t token, Image* verification = nullptr);
  Image readback();  // Explicit verification/desktop preview only; direct output never calls it.
  std::string renderer() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace headunit::theme::graphics
