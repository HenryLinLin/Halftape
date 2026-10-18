import AppKit
import ScreenCaptureKit
import VideoToolbox

enum MirrorState: Equatable {
    case off, noSource, needsPermission, areaNotSet, waiting, hiddenInteracting, hiddenCovered, windowOffScreen, mirroring(Int), failed(String)
    var text: String {
        switch self {
        case .off: return "Off"
        case .noSource: return "Start capture first."
        case .needsPermission: return "Needs Screen Recording access. Allow Halftape, then relaunch it."
        case .areaNotSet: return "No lyrics area for this window size yet."
        case .waiting: return "Ready. Shows once Sing is playing."
        case .hiddenInteracting: return "Hidden while you use the source window."
        case .hiddenCovered: return "Hidden while another window covers the lyrics."
        case .windowOffScreen: return "The source window is not on screen."
        case .mirroring(let ms): return "Mirroring, lyrics \(ms) ms behind."
        case .failed(let reason): return "Capture failed. \(reason)"
        }
    }
    var symbol: String {
        switch self {
        case .mirroring: return "checkmark.circle.fill"
        case .needsPermission, .failed: return "exclamationmark.triangle.fill"
        case .hiddenInteracting, .hiddenCovered: return "eye.slash"
        default: return "circle"
        }
    }
}

@MainActor final class LyricsMirror: NSObject, ObservableObject {
    @Published var enabled = UserDefaults.standard.bool(forKey: "mirrorEnabled") {
        didSet {
            UserDefaults.standard.set(enabled, forKey: "mirrorEnabled")
            if !enabled { stopStream(); panel.orderOut(nil) } else if !permitted { permitted = CGRequestScreenCaptureAccess() }
        }
    }
    @Published var showBadge = UserDefaults.standard.object(forKey: "mirrorBadge") as? Bool ?? true {
        didSet { UserDefaults.standard.set(showBadge, forKey: "mirrorBadge"); badge.isHidden = !showBadge }
    }
    @Published private(set) var state = MirrorState.off
    @Published private(set) var areaSet = false
    @Published private(set) var permitted = CGPreflightScreenCaptureAccess()

    private struct Target { var windowID: CGWindowID; var frame: CGRect; var scale: CGFloat; var occluded: Bool }

    private let panel: NSPanel
    private let imageLayer = CALayer()
    private let outline = CALayer()
    private let badge = CATextLayer()
    private let areaPanel = OutlinePanel()
    private var timer: Timer?
    private var tick = 0
    private var pid: Int32 = 0
    private var bundle = ""
    private var delay = 0.3
    private var playing = false
    private var epoch: UInt64 = 0
    private var target: Target?
    private var area: CGRect?
    private var stream: SCStream?
    private var streamKey = ""
    private var starting = false
    private var announce = true
    private let queue = DispatchQueue(label: "org.henrylin.Halftape.mirror")
    nonisolated let frames = FrameRing()
    private var lastMouse = NSEvent.mouseLocation
    private var lastMove = CACurrentMediaTime()
    private var setup: SetupWindow?

    override init() {
        panel = NSPanel(contentRect: .zero, styleMask: [.borderless, .nonactivatingPanel], backing: .buffered, defer: true)
        super.init()
        panel.isReleasedWhenClosed = false
        panel.level = .floating
        panel.isOpaque = false
        panel.backgroundColor = .clear
        panel.hasShadow = false
        panel.ignoresMouseEvents = true
        panel.hidesOnDeactivate = false
        panel.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .stationary, .ignoresCycle]
        panel.contentView?.wantsLayer = true
        imageLayer.contentsGravity = .resize
        imageLayer.magnificationFilter = .linear
        outline.borderColor = NSColor.systemOrange.cgColor
        outline.borderWidth = 3
        outline.opacity = 0
        badge.string = ""
        badge.fontSize = 12
        badge.font = NSFont.systemFont(ofSize: 12, weight: .medium)
        badge.foregroundColor = NSColor.white.cgColor
        badge.backgroundColor = NSColor.black.withAlphaComponent(0.55).cgColor
        badge.cornerRadius = 8
        badge.alignmentMode = .center
        badge.isHidden = !showBadge
        for layer in [imageLayer, outline, badge] { panel.contentView?.layer?.addSublayer(layer) }
    }

    func update(pid: Int32, bundle: String, delay: Double, singing: Bool, playing: Bool, epoch: UInt64) {
        self.pid = pid; self.bundle = bundle; self.playing = playing
        self.delay = max(0.05, min(2, delay))
        if epoch != self.epoch { self.epoch = epoch; flush() }
        let wanted = enabled && pid > 0 && singing
        if wanted && timer == nil {
            timer = Timer.scheduledTimer(withTimeInterval: 0.05, repeats: true) { [weak self] _ in MainActor.assumeIsolated { self?.step() } }
            RunLoop.main.add(timer!, forMode: .common)
        } else if !wanted, let running = timer {
            running.invalidate(); timer = nil; stopStream(); panel.orderOut(nil); target = nil
        }
        if !wanted {
            if !enabled { set(.off) }
            else if pid <= 0 { set(.noSource) }
            else if !permitted { set(.needsPermission) }
            else if let located = locate() { target = located.window; refreshArea(); set(areaSet ? .waiting : .areaNotSet) }
            else { target = nil; set(.windowOffScreen) }
        }
    }

    private func set(_ next: MirrorState) { if state != next { state = next } }

    func clearArea() {
        guard let target else { return }
        UserDefaults.standard.removeObject(forKey: areaKey(for: target.frame.size))
        area = nil; areaSet = false; stopStream(); panel.orderOut(nil); areaPanel.orderOut(nil)
    }

    func showArea() {
        if target == nil, let located = locate() { target = located.window }
        guard let target else { set(.windowOffScreen); return }
        refreshArea()
        guard let area else { set(.areaNotSet); return }
        areaPanel.show(at: screenRect(for: area, in: target), label: "Lyrics area", seconds: 2)
    }

    var settingUp: Bool { setup != nil }

    func cancelSetup() { setup?.cancel() }

    func openPermissionSettings() {
        if let url = URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenCapture") { NSWorkspace.shared.open(url) }
    }

    func beginSetup() {
        guard setup == nil else { return }
        if target == nil, let located = locate() { target = located.window }
        guard let target else { set(.windowOffScreen); return }
        let frame = nsRect(fromCG: target.frame)
        let screen = NSScreen.screens.first { $0.frame.intersects(frame) } ?? NSScreen.main
        guard let screen else { return }
        let window = SetupWindow(screen: screen, source: frame) { [weak self] picked in
            guard let self else { return }
            self.setup = nil
            self.objectWillChange.send()
            guard let picked, let t = self.target else { return }
            let local = CGRect(x: picked.minX - frame.minX, y: frame.maxY - picked.maxY, width: picked.width, height: picked.height).integral
            UserDefaults.standard.set([local.minX, local.minY, local.width, local.height], forKey: self.areaKey(for: t.frame.size))
            self.area = local; self.areaSet = true; self.stopStream()
            self.set(.waiting)
            self.areaPanel.show(at: self.screenRect(for: local, in: t), label: "Saved. Lyrics in this box play late to match the audio.", seconds: 2.5)
        }
        setup = window
        NSApp.activate(ignoringOtherApps: true)
        window.makeKeyAndOrderFront(nil)
        objectWillChange.send()
    }

    private func areaKey(for size: CGSize) -> String { "mirror.area.\(bundle).\(Int(size.width.rounded()))x\(Int(size.height.rounded()))" }

    private func refreshArea() {
        guard let target else { area = nil; areaSet = false; return }
        if let saved = UserDefaults.standard.array(forKey: areaKey(for: target.frame.size)) as? [Double], saved.count == 4 {
            area = CGRect(x: saved[0], y: saved[1], width: saved[2], height: saved[3]); areaSet = true
        } else { area = nil; areaSet = false }
    }

    private func screenRect(for area: CGRect, in t: Target) -> NSRect {
        nsRect(fromCG: CGRect(x: t.frame.minX + area.minX, y: t.frame.minY + area.minY, width: area.width, height: area.height))
    }

    private func step() {
        tick += 1
        if tick % 2 == 0 { track() }
        present()
    }

    // The source app's main window and the frames of every window stacked in front of it.
    private func locate() -> (window: Target, above: [CGRect])? {
        guard pid > 0, let list = CGWindowListCopyWindowInfo([.optionOnScreenOnly, .excludeDesktopElements], kCGNullWindowID) as? [[String: Any]] else { return nil }
        var found: Target?
        var above: [CGRect] = []
        let ours = Set([panel.windowNumber, areaPanel.windowNumber].filter { $0 > 0 }.map { CGWindowID($0) })
        for entry in list {
            guard found == nil,
                  let number = entry[kCGWindowNumber as String] as? CGWindowID, !ours.contains(number),
                  let layer = entry[kCGWindowLayer as String] as? Int, layer == 0,
                  let dict = entry[kCGWindowBounds as String] as? NSDictionary, let bounds = CGRect(dictionaryRepresentation: dict as CFDictionary) else { continue }
            if let owner = entry[kCGWindowOwnerPID as String] as? Int32, owner == pid {
                guard bounds.width > 200, bounds.height > 200 else { continue }
                let nsFrame = nsRect(fromCG: bounds)
                let scale = NSScreen.screens.first { $0.frame.intersects(nsFrame) }?.backingScaleFactor ?? 2
                found = Target(windowID: number, frame: bounds, scale: scale, occluded: false)
            } else {
                above.append(bounds)
            }
        }
        guard let found else { return nil }
        return (found, above)
    }

    private func track() {
        guard let located = locate() else {
            target = nil; area = nil; areaSet = false; stopStream(); panel.orderOut(nil)
            set(.windowOffScreen)
            return
        }
        var t = located.window
        target = t
        refreshArea()
        if let area { t.occluded = located.above.contains { $0.intersects(CGRect(x: t.frame.minX + area.minX, y: t.frame.minY + area.minY, width: area.width, height: area.height)) } }
        target = t
        let allowed = CGPreflightScreenCaptureAccess()
        if allowed != permitted { permitted = allowed }
        guard permitted else { set(.needsPermission); stopStream(); return }
        guard let area else { set(.areaNotSet); stopStream(); return }
        let wantedKey = "\(t.windowID):\(Int(t.frame.width))x\(Int(t.frame.height)):\(area):\(t.scale)"
        if wantedKey != streamKey { startStream(window: t.windowID, area: area, scale: t.scale, key: wantedKey) }
    }

    private func interacting(_ t: Target) -> Bool {
        let now = CACurrentMediaTime()
        let mouse = NSEvent.mouseLocation
        if mouse != lastMouse { lastMouse = mouse; lastMove = now }
        let inside = nsRect(fromCG: t.frame).contains(mouse)
        if inside && now - lastMove < 1 { return true }
        if NSWorkspace.shared.frontmostApplication?.processIdentifier == pid {
            let keys = CGEventSource.secondsSinceLastEventType(.combinedSessionState, eventType: .keyDown)
            let scroll = CGEventSource.secondsSinceLastEventType(.combinedSessionState, eventType: .scrollWheel)
            if keys < 1 || (inside && scroll < 1) { return true }
        }
        return false
    }

    private func present() {
        guard let t = target, let area, permitted, stream != nil else { return }
        let shot = frames.take(before: CACurrentMediaTime() - delay)
        if let shot { CATransaction.begin(); CATransaction.setDisableActions(true); imageLayer.contents = shot; CATransaction.commit() }
        let haveImage = shot != nil || imageLayer.contents != nil
        guard playing, haveImage else { panel.orderOut(nil); announce = true; set(.waiting); return }
        guard !t.occluded else { panel.orderOut(nil); announce = true; set(.hiddenCovered); return }
        guard !interacting(t) else { panel.orderOut(nil); set(.hiddenInteracting); return }
        let frame = screenRect(for: area, in: t)
        if panel.frame != frame { panel.setFrame(frame, display: false) }
        let ms = Int((delay * 1000).rounded())
        CATransaction.begin(); CATransaction.setDisableActions(true)
        let bounds = panel.contentView?.bounds ?? .zero
        if imageLayer.contentsScale != t.scale { imageLayer.contentsScale = t.scale; badge.contentsScale = t.scale }
        if imageLayer.frame != bounds { imageLayer.frame = bounds; outline.frame = bounds }
        let label = "Lyrics \(ms) ms behind"
        if badge.string as? String != label { badge.string = label }
        let width = CGFloat(label.count) * 7 + 20
        badge.frame = CGRect(x: bounds.maxX - width - 10, y: 10, width: width, height: 22)
        CATransaction.commit()
        if !panel.isVisible { panel.orderFrontRegardless() }
        if announce { announce = false; flashOutline() }
        set(.mirroring(ms))
    }

    private func flashOutline() {
        let fade = CABasicAnimation(keyPath: "opacity")
        fade.fromValue = 1; fade.toValue = 0; fade.duration = 1.5
        outline.add(fade, forKey: "flash")
    }

    private func flush() {
        frames.flush(at: CACurrentMediaTime())
        CATransaction.begin(); CATransaction.setDisableActions(true); imageLayer.contents = nil; CATransaction.commit()
        panel.orderOut(nil)
        announce = true
    }

    private func startStream(window: CGWindowID, area: CGRect, scale: CGFloat, key: String) {
        guard !starting else { return }
        starting = true
        stopStream()
        streamKey = key
        Task { [weak self] in
            defer { Task { @MainActor in self?.starting = false } }
            do {
                let content = try await SCShareableContent.excludingDesktopWindows(false, onScreenWindowsOnly: true)
                guard let target = content.windows.first(where: { $0.windowID == window }) else { return }
                let filter = SCContentFilter(desktopIndependentWindow: target)
                let config = SCStreamConfiguration()
                config.sourceRect = area
                config.width = Int(area.width * scale)
                config.height = Int(area.height * scale)
                config.scalesToFit = false
                config.showsCursor = false
                config.pixelFormat = kCVPixelFormatType_32BGRA
                config.minimumFrameInterval = CMTime(value: 1, timescale: 20)
                config.queueDepth = 5
                guard let self else { return }
                let stream = SCStream(filter: filter, configuration: config, delegate: self)
                try stream.addStreamOutput(self, type: .screen, sampleHandlerQueue: self.queue)
                try await stream.startCapture()
                await MainActor.run { if self.streamKey == key { self.stream = stream } else { stream.stopCapture { _ in } } }
            } catch {
                await MainActor.run { self?.set(.failed(error.localizedDescription)) }
            }
        }
    }

    private func stopStream() {
        if let running = stream { running.stopCapture { _ in } }
        stream = nil; streamKey = ""
        flush()
    }

    private func nsRect(fromCG rect: CGRect) -> NSRect {
        let height = NSScreen.screens.first?.frame.height ?? 0
        return NSRect(x: rect.minX, y: height - rect.maxY, width: rect.width, height: rect.height)
    }
}

extension LyricsMirror: SCStreamDelegate, SCStreamOutput {
    nonisolated func stream(_ stream: SCStream, didStopWithError error: Error) {
        Task { @MainActor in
            if self.stream === stream { self.stream = nil; self.streamKey = ""; self.set(.failed(error.localizedDescription)) }
        }
    }
    nonisolated func stream(_ stream: SCStream, didOutputSampleBuffer sample: CMSampleBuffer, of type: SCStreamOutputType) {
        guard type == .screen, sample.isValid, let pixels = sample.imageBuffer else { return }
        if let attachments = CMSampleBufferGetSampleAttachmentsArray(sample, createIfNecessary: false) as? [[SCStreamFrameInfo: Any]],
           let status = attachments.first?[.status] as? Int, status != SCFrameStatus.complete.rawValue { return }
        var image: CGImage?
        VTCreateCGImageFromCVPixelBuffer(pixels, options: nil, imageOut: &image)
        guard let image else { return }
        frames.add(image, at: CMTimeGetSeconds(sample.presentationTimeStamp))
    }
}

final class FrameRing: @unchecked Sendable {
    private struct Shot { let time: Double; let image: CGImage }
    private let lock = NSLock()
    private var shots: [Shot] = []
    private var flushedAt = 0.0
    func add(_ image: CGImage, at time: Double) {
        lock.lock(); defer { lock.unlock() }
        guard time >= flushedAt else { return }
        shots.append(Shot(time: time, image: image))
        if shots.count > 30 { shots.removeFirst(shots.count - 30) }
    }
    // Newest frame old enough to show; older ones are dropped.
    func take(before cutoff: Double) -> CGImage? {
        lock.lock(); defer { lock.unlock() }
        guard let index = shots.lastIndex(where: { $0.time <= cutoff }) else { return nil }
        shots.removeFirst(index)
        return shots[0].image
    }
    func flush(at time: Double) {
        lock.lock(); defer { lock.unlock() }
        shots.removeAll(); flushedAt = time
    }
}

// Click-through orange outline with a label, used to point at the saved area.
@MainActor private final class OutlinePanel: NSPanel {
    private let text = CATextLayer()
    private var hideWork: DispatchWorkItem?
    init() {
        super.init(contentRect: .zero, styleMask: [.borderless, .nonactivatingPanel], backing: .buffered, defer: true)
        isReleasedWhenClosed = false
        level = .floating
        isOpaque = false
        backgroundColor = .clear
        hasShadow = false
        ignoresMouseEvents = true
        collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .stationary, .ignoresCycle]
        contentView?.wantsLayer = true
        contentView?.layer?.borderColor = NSColor.systemOrange.cgColor
        contentView?.layer?.borderWidth = 3
        text.fontSize = 13
        text.font = NSFont.systemFont(ofSize: 13, weight: .medium)
        text.foregroundColor = NSColor.white.cgColor
        text.backgroundColor = NSColor.systemOrange.cgColor
        text.cornerRadius = 6
        text.alignmentMode = .center
        contentView?.layer?.addSublayer(text)
    }
    func show(at frame: NSRect, label: String, seconds: Double) {
        hideWork?.cancel()
        setFrame(frame, display: false)
        CATransaction.begin(); CATransaction.setDisableActions(true)
        text.contentsScale = screen?.backingScaleFactor ?? 2
        text.string = label
        let width = min(frame.width - 12, CGFloat(label.count) * 7.5 + 24)
        text.frame = CGRect(x: 6, y: frame.height - 32, width: width, height: 24)
        CATransaction.commit()
        orderFrontRegardless()
        let work = DispatchWorkItem { [weak self] in self?.orderOut(nil) }
        hideWork = work
        DispatchQueue.main.asyncAfter(deadline: .now() + seconds, execute: work)
    }
}

private final class SetupWindow: NSWindow {
    private var done: ((NSRect?) -> Void)?
    private let picker: SetupView
    private var keyMonitor: Any?
    private var deactivation: Any?
    init(screen: NSScreen, source: NSRect, done: @escaping (NSRect?) -> Void) {
        self.done = done
        picker = SetupView(source: source)
        super.init(contentRect: screen.frame, styleMask: .borderless, backing: .buffered, defer: false)
        level = .modalPanel
        isReleasedWhenClosed = false
        isOpaque = false
        backgroundColor = .clear
        // Explicitly false, otherwise AppKit lets clicks fall through the lightly tinted source area.
        ignoresMouseEvents = false
        collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary]
        contentView = picker
        picker.finished = { [weak self] rect in self?.finish(rect) }
        keyMonitor = NSEvent.addLocalMonitorForEvents(matching: .keyDown) { [weak self] event in
            guard event.keyCode == 53 else { return event }
            MainActor.assumeIsolated { self?.finish(nil) }
            return nil
        }
        deactivation = NotificationCenter.default.addObserver(forName: NSApplication.didResignActiveNotification, object: nil, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated { self?.finish(nil) }
        }
    }
    override var canBecomeKey: Bool { true }
    override func keyDown(with event: NSEvent) { if event.keyCode == 53 { finish(nil) } }
    func cancel() { finish(nil) }
    private func finish(_ rect: NSRect?) {
        guard let done else { return }
        self.done = nil
        if let keyMonitor { NSEvent.removeMonitor(keyMonitor) }
        if let deactivation { NotificationCenter.default.removeObserver(deactivation) }
        keyMonitor = nil; deactivation = nil
        orderOut(nil)
        DispatchQueue.main.async { done(rect) }
    }
}

private final class SetupView: NSView {
    var finished: ((NSRect?) -> Void)?
    private let source: NSRect
    private var start: NSPoint?
    private var current: NSRect?
    init(source: NSRect) { self.source = source; super.init(frame: .zero) }
    required init?(coder: NSCoder) { nil }
    override var acceptsFirstResponder: Bool { true }
    private func screenPoint(_ event: NSEvent) -> NSPoint { window?.convertPoint(toScreen: event.locationInWindow) ?? event.locationInWindow }
    override func mouseDown(with event: NSEvent) {
        let point = screenPoint(event)
        guard source.contains(point) else { finished?(nil); return }
        start = point; current = nil; needsDisplay = true
    }
    override func mouseDragged(with event: NSEvent) {
        guard let start else { return }
        let p = screenPoint(event)
        current = NSRect(x: min(start.x, p.x), y: min(start.y, p.y), width: abs(p.x - start.x), height: abs(p.y - start.y)).intersection(source)
        needsDisplay = true
    }
    override func mouseUp(with event: NSEvent) {
        guard let rect = current, rect.width > 20, rect.height > 20 else { start = nil; current = nil; needsDisplay = true; return }
        finished?(rect)
    }
    override func draw(_ dirtyRect: NSRect) {
        guard let window else { return }
        NSColor.black.withAlphaComponent(0.5).setFill()
        bounds.fill()
        let local = window.convertFromScreen(source)
        NSColor.black.withAlphaComponent(0.04).setFill()
        local.fill(using: .copy)
        NSColor.white.withAlphaComponent(0.9).setStroke()
        NSBezierPath(rect: local).stroke()
        if let current {
            let box = window.convertFromScreen(current)
            NSColor.systemOrange.withAlphaComponent(0.15).setFill()
            box.fill()
            NSColor.systemOrange.setStroke()
            let path = NSBezierPath(rect: box); path.lineWidth = 2; path.stroke()
        }
        let text = current == nil ? "Drag a box over the lyrics in the bright window. Click outside it or press Escape to cancel." : "Release to save this box."
        let attributes: [NSAttributedString.Key: Any] = [.font: NSFont.systemFont(ofSize: 16, weight: .semibold), .foregroundColor: NSColor.white]
        let size = text.size(withAttributes: attributes)
        let banner = NSRect(x: bounds.midX - size.width / 2 - 18, y: bounds.maxY - 70, width: size.width + 36, height: 40)
        NSColor.systemOrange.setFill()
        NSBezierPath(roundedRect: banner, xRadius: 10, yRadius: 10).fill()
        text.draw(at: NSPoint(x: banner.minX + 18, y: banner.minY + 10), withAttributes: attributes)
    }
}
