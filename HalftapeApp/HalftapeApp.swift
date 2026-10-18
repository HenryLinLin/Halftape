import SwiftUI
import AppKit
import Carbon
import Combine
import UniformTypeIdentifiers

@main
struct HalftapeApp: App {
    @StateObject private var model = PracticeModel()
    var body: some Scene {
        Window("Halftape", id: "practice") { PracticeView().environmentObject(model).frame(minWidth: 900, minHeight: 650) }
            .defaultSize(width: 1120, height: 780)
        MenuBarExtra("\(model.capturing ? "Capturing" : "Halftape") · \(Int(model.status.buffered))s", systemImage: "waveform") { MenuContent().environmentObject(model) }
            .menuBarExtraStyle(.window)
        Window("Sing", id: "sing") { SingPanel().environmentObject(model).frame(minWidth: 400, minHeight: 460) }
            .defaultSize(width: 400, height: 460)
    }
}

private let accent = Color.accentColor
private let canvas = Color(nsColor: .windowBackgroundColor)
private func cString<T>(_ bytes: T) -> String {
    withUnsafePointer(to: bytes) { ptr in
        ptr.withMemoryRebound(to: CChar.self, capacity: MemoryLayout<T>.size) { String(cString: $0) }
    }
}

@MainActor final class PracticeModel: ObservableObject {
    let ws: OpaquePointer
    @Published var status = WSStatus()
    @Published var lowPowerMode = ProcessInfo.processInfo.isLowPowerModeEnabled
    @Published var sources: [WSSource] = []
    var armedSourceName: String { sources.first(where: { $0.id == sourceId }).map { cString($0.name) } ?? "Halftape" }
    var capturedSourceName: String { let name = cString(status.capture_name); return name.isEmpty ? armedSourceName : name }
    var capturing: Bool { status.capturing != 0 }
    var canGrab: Bool { status.buffered >= 0.1 && status.sing == 0 }
    var grabLabel: String {
        guard status.buffered >= 0.1 else { return "Grab last \(Int(grabSeconds))s" }
        let available = min(grabSeconds, status.buffered)
        return available < 1 ? String(format: "Grab last %.1fs", available) : "Grab last \(Int(available))s"
    }
    var captureLabel: String {
        let duration = String(format: "%02d:%02d", Int(status.buffered) / 60, Int(status.buffered) % 60)
        if capturing { return "Capturing \(capturedSourceName) · \(duration) buffered" }
        if status.buffered >= 0.1 { return "Capture stopped · \(duration) saved from \(capturedSourceName)" }
        return "Not capturing"
    }
    @Published var history: [WSHistory] = []
    @Published var waveform: [Float] = []
    @Published var beats: [Double] = []
    @Published var sourceId: UInt32 = 0
    @Published var snap = true
    @Published var paused = false
    @Published var trainer = false
    @Published var countIn = false
    @Published var chooser = false
    @Published var grabSeconds = 30.0
    @Published var heldGrab = false
    @Published var modelSix = UserDefaults.standard.string(forKey: "modelSix") ?? ""
    @Published var modelFour = UserDefaults.standard.string(forKey: "modelFour") ?? ""
    @Published var modelSing = UserDefaults.standard.string(forKey: "modelSing") ?? ""
    @Published var modelClean = UserDefaults.standard.string(forKey: "modelClean") ?? ""
    @Published var singClean = UserDefaults.standard.object(forKey: "singClean") as? Bool ?? true
    @Published var singLookahead = UserDefaults.standard.object(forKey: "singLookahead") as? Int ?? 2 {
        didSet { UserDefaults.standard.set(singLookahead, forKey: "singLookahead"); ws_sing_lookahead(ws, Int32(singLookahead)) }
    }
    let mirror = LyricsMirror()
    private var mirrorObserver: AnyCancellable?
    @Published var gains = Array(repeating: 1.0, count: 6)
    @Published var mutes = Array(repeating: false, count: 6)
    @Published var solos = Array(repeating: false, count: 6)
    private var poller: Timer?
    private var holdTimer: Timer?
    private var lastGrabDate = Date.distantPast
    private var lastGrabRequest = 30.0
    private var hotkeys: HotKeyController?
    private var closed = false
    private var openPracticeWindow: (() -> Void)?
    private var openSingWindow: (() -> Void)?
    private var terminationObserver: NSObjectProtocol?
    init() {
        guard let created = ws_create() else {
            let alert = NSAlert()
            alert.messageText = "Halftape could not start"
            alert.informativeText = "The audio practice engine could not be initialized. Please relaunch the app."
            alert.alertStyle = .critical
            alert.runModal()
            fatalError("Halftape backend initialization failed")
        }
        ws = created
        terminationObserver = NotificationCenter.default.addObserver(forName: NSApplication.willTerminateNotification, object: nil, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated { self?.close() }
        }
        hotkeys = HotKeyController(model: self)
        mirrorObserver = mirror.objectWillChange.sink { [weak self] _ in self?.objectWillChange.send() }
        discoverModelsIfNeeded()
        setModels()
        setSingModel()
        ws_sing_lookahead(ws, Int32(singLookahead))
        refreshSources()
        ws_count_in(ws, countIn ? 1 : 0)
        poller = Timer.scheduledTimer(withTimeInterval: 0.15, repeats: true) { [weak self] _ in guard let model = self else { return }; MainActor.assumeIsolated { model.poll() } }
    }
    deinit { poller?.invalidate(); if !closed { ws_destroy(ws) } }
    func close() {
        guard !closed else { return }
        closed = true; poller?.invalidate(); holdTimer?.invalidate(); hotkeys?.dispose(); ws_destroy(ws)
        if let terminationObserver { NotificationCenter.default.removeObserver(terminationObserver); self.terminationObserver = nil }
    }
    func setWindowOpeners(practice: @escaping () -> Void, sing: @escaping () -> Void) {
        openPracticeWindow = practice; openSingWindow = sing
    }
    func revealPractice() { NSApp.activate(ignoringOtherApps: true); openPracticeWindow?() }
    func revealSing() { NSApp.activate(ignoringOtherApps: true); openSingWindow?() }
    func refreshSources() {
        var rows = Array(repeating: WSSource(), count: 32)
        let n = ws_sources(ws, &rows, Int32(rows.count))
        sources = Array(rows.prefix(max(0, Int(n))))
        if sourceId == 0 { sourceId = sources.first(where: { $0.active != 0 })?.id ?? 0 }
    }
    var isSinging: Bool { status.sing != 0 }
    func poll() {
        guard !closed else { return }
        ws_status(ws, &status)
        let lowPowerModeNow = ProcessInfo.processInfo.isLowPowerModeEnabled
        if lowPowerMode != lowPowerModeNow { lowPowerMode = lowPowerModeNow }
        ws_sing_steady(ws, mirror.enabled ? 1 : 0)
        mirror.update(pid: status.capture_pid, bundle: cString(status.capture_bundle), delay: status.sing_latency, singing: status.sing != 0, playing: status.sing_state == 3, epoch: status.sing_epoch)
        var wave = Array(repeating: Float(0), count: 1024)
        let wn = ws_waveform(ws, &wave, Int32(wave.count)); waveform = Array(wave.prefix(max(0, Int(wn))))
        var grid = Array(repeating: Double(0), count: 512)
        let bn = ws_beats(ws, &grid, Int32(grid.count)); beats = Array(grid.prefix(max(0, Int(bn))))
        var rows = Array(repeating: WSHistory(), count: 20)
        let hn = ws_history(ws, &rows, Int32(rows.count)); history = Array(rows.prefix(max(0, Int(hn))))
    }
    func arm() { guard !capturing, sourceId != 0 else { return }; ws_arm(ws, sourceId); poll() }
    func stopCapture() { guard capturing else { return }; ws_stop_capture(ws); poll() }
    func updateStem(_ index:Int) { guard status.stems_ready > 0, !isSinging else { return }; ws_stem(ws, Int32(index), Float(gains[index]), mutes[index] ? 1:0, solos[index] ? 1:0) }
    func toggleSing() { guard capturing || status.sing != 0 else { return }; ws_sing(ws, status.sing == 0 ? 1 : 0); poll() }
    var hasSource: Bool { status.capture_pid > 0 }
    func transport(_ command: Int32) { guard hasSource else { return }; ws_source_transport(ws, command); poll() }
    func grab(_ seconds: Double? = nil) {
        guard canGrab else { return }
        let now = Date()
        let requested = seconds ?? (now.timeIntervalSince(lastGrabDate) < 1 ? min(180, lastGrabRequest + 30) : grabSeconds)
        let next = min(status.buffered, requested)
        guard next >= 0.1 else { return }
        lastGrabDate = now; lastGrabRequest = requested; ws_grab(ws, next); poll(); chooser = false
    }
    func togglePlay() { guard status.duration >= 0.1 else { return }; if isSinging { ws_play(ws, 1) } else { ws_play(ws, status.playing == 0 ? 1 : 0) }; poll() }
    func seek(_ value: Double) { guard !isSinging else { return }; ws_seek(ws, max(0, min(status.duration, value))); poll() }
    func setSpeed(_ value: Double) { guard !isSinging else { return }; ws_speed(ws, Float(min(1.5, max(0.25, value)))) }
    func setPitch(_ value: Double) { guard !isSinging else { return }; ws_pitch(ws, Float(min(13, max(-13, value)))) }
    func loopA(_ a: Double, _ b: Double) { guard !isSinging else { return }; ws_loop(ws, a, b, snap ? 1 : 0) }
    func loopEnabled(_ enabled: Bool) { ws_loop_enabled(ws, enabled ? 1 : 0); poll() }
    func modelPicker(six: Bool) {
        let p = NSOpenPanel()
        p.allowedContentTypes = [UTType(filenameExtension: "onnx") ?? .data, UTType(filenameExtension: "bin") ?? .data]
        p.allowsMultipleSelection = false
        p.message = "Choose an ONNX model (preferred) or BIN model"
        p.begin { [weak self] response in guard response == .OK, let path = p.url?.path else { return }
            Task { @MainActor in
                if six { self?.modelSix = path; UserDefaults.standard.set(path, forKey: "modelSix") }
                else { self?.modelFour = path; UserDefaults.standard.set(path, forKey: "modelFour") }
                self?.setModels()
            }
        }
    }
    func singModelPicker() {
        let p = NSOpenPanel()
        p.allowedContentTypes = [UTType(filenameExtension: "onnx") ?? .data]
        p.allowsMultipleSelection = false
        p.message = singClean ? "Choose the clean Sing model (umxl-vocals.onnx)" : "Choose the StemGenRT hop-128 ONNX model"
        p.begin { [weak self] response in guard response == .OK, let path = p.url?.path else { return }
            Task { @MainActor in
                guard let self else { return }
                if self.singClean { self.modelClean = path; UserDefaults.standard.set(path, forKey: "modelClean") }
                else { self.modelSing = path; UserDefaults.standard.set(path, forKey: "modelSing") }
                self.setSingModel()
            }
        }
    }
    func setSingMode(_ clean: Bool) {
        guard status.sing == 0 else { return }
        singClean = clean; UserDefaults.standard.set(clean, forKey: "singClean")
        setSingModel(); poll()
    }
    func setModels() { modelSix.withCString { a in modelFour.withCString { b in ws_set_model(ws, a, b) } } }
    func setSingModel() { (singClean ? modelClean : modelSing).withCString { ws_set_sing_model(ws, $0) } }
    private func discoverModelsIfNeeded() {
        let fileManager = FileManager.default
        var folders: [URL] = []
        if let support = fileManager.urls(for: .applicationSupportDirectory, in: .userDomainMask).first {
            folders.append(support.appendingPathComponent("Halftape/models", isDirectory: true))
        }
        let devModels = Bundle.main.bundleURL
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .appendingPathComponent(".cache/models", isDirectory: true)
        folders.append(devModels)
        func usable(_ path: String) -> Bool {
            guard !path.isEmpty, fileManager.isReadableFile(atPath: path) else { return false }
            var isDirectory: ObjCBool = false
            return fileManager.fileExists(atPath: path, isDirectory: &isDirectory) && !isDirectory.boolValue
        }
        if !usable(modelSix) {
            modelSix = folders.map { $0.appendingPathComponent("htdemucs_6s.onnx").path }.first(where: usable) ?? ""
            UserDefaults.standard.set(modelSix, forKey: "modelSix")
        }
        if !usable(modelSing) {
            modelSing = folders.map { $0.appendingPathComponent("stemgenrt-hop128.onnx").path }.first(where: usable) ?? ""
            UserDefaults.standard.set(modelSing, forKey: "modelSing")
        }
        if !usable(modelClean) {
            modelClean = folders.map { $0.appendingPathComponent("umxl-vocals.onnx").path }.first(where: usable) ?? ""
            UserDefaults.standard.set(modelClean, forKey: "modelClean")
        }
        if !usable(modelFour) {
            modelFour = folders.map { $0.appendingPathComponent("htdemucs.onnx").path }.first(where: usable) ?? ""
            UserDefaults.standard.set(modelFour, forKey: "modelFour")
        }
    }
    func startHold() { heldGrab = true; holdTimer?.invalidate(); holdTimer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: false) { [weak self] _ in guard let model = self else { return }; MainActor.assumeIsolated { if model.heldGrab { model.chooser = true } } } }
    func endHold() { let wasHeld = heldGrab; heldGrab = false; holdTimer?.invalidate(); if wasHeld && !chooser { grab() } }
}

@MainActor private final class HotKeyController {
    private weak var model: PracticeModel?
    private var gHotKey: EventHotKeyRef?
    private var kHotKey: EventHotKeyRef?
    private var transportKeys: [EventHotKeyRef?] = []
    private var handler: EventHandlerRef?
    init(model: PracticeModel) {
        self.model = model
        var specs = [EventTypeSpec(eventClass: OSType(kEventClassKeyboard), eventKind: UInt32(kEventHotKeyPressed)), EventTypeSpec(eventClass: OSType(kEventClassKeyboard), eventKind: UInt32(kEventHotKeyReleased))]
        let status = InstallEventHandler(GetEventDispatcherTarget(), hotKeyEvent, specs.count, &specs, Unmanaged.passUnretained(self).toOpaque(), &handler)
        if status == noErr {
            let g = EventHotKeyID(signature: OSType(0x57534844), id: 1)
            let k = EventHotKeyID(signature: OSType(0x57534844), id: 2)
            RegisterEventHotKey(UInt32(kVK_ANSI_G), UInt32(controlKey | optionKey), g, GetEventDispatcherTarget(), 0, &gHotKey)
            RegisterEventHotKey(UInt32(kVK_ANSI_K), UInt32(controlKey | optionKey), k, GetEventDispatcherTarget(), 0, &kHotKey)
            for (index, code) in [kVK_ANSI_P, kVK_Space, kVK_ANSI_N].enumerated() {
                var ref: EventHotKeyRef?
                RegisterEventHotKey(UInt32(code), UInt32(controlKey | optionKey), EventHotKeyID(signature: OSType(0x57534844), id: UInt32(3 + index)), GetEventDispatcherTarget(), 0, &ref)
                transportKeys.append(ref)
            }
        }
    }
    func event(_ id: UInt32, pressed: Bool) {
        if id == 1 { if pressed, let m = model { m.revealPractice(); m.startHold() } else { model?.endHold() } }
        else if id == 2, pressed, let m = model { m.toggleSing(); m.revealSing() }
        else if id >= 3, id <= 5, pressed, let m = model { m.transport(Int32(id - 3)) }
    }
    fileprivate func dispose() {
        if let gHotKey { UnregisterEventHotKey(gHotKey) }; if let kHotKey { UnregisterEventHotKey(kHotKey) }
        for key in transportKeys { if let key { UnregisterEventHotKey(key) } }
        if let handler { RemoveEventHandler(handler) }
    }
}

private let hotKeyEvent: EventHandlerProcPtr = { _, event, userData in
    guard let event, let userData else { return noErr }
    var hotKey = EventHotKeyID()
    let result = GetEventParameter(event, EventParamName(kEventParamDirectObject), EventParamType(typeEventHotKeyID), nil, MemoryLayout<EventHotKeyID>.size, nil, &hotKey)
    if result == noErr { let pressed = GetEventKind(event) == UInt32(kEventHotKeyPressed); let controller = Unmanaged<HotKeyController>.fromOpaque(userData).takeUnretainedValue(); let id = hotKey.id; Task { @MainActor in controller.event(id, pressed: pressed) } }
    return noErr
}


struct MenuContent: View {
    @EnvironmentObject var m: PracticeModel
    @Environment(\.openWindow) private var openWindow
    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack { Text("Halftape").font(.headline); Spacer(); Circle().fill(m.capturing ? .green : .secondary).frame(width: 8,height: 8) }
            Text(m.captureLabel).font(.caption).foregroundStyle(.secondary)
            Text(cString(m.status.message)).font(.caption2).foregroundStyle(.secondary).lineLimit(2)
            HStack { Button { m.revealPractice(); m.grab() } label: { Label(m.grabLabel, systemImage: "arrow.down.to.line") }.disabled(!m.canGrab); Button { m.togglePlay() } label: { Label(m.isSinging ? "Play Clip" : (m.status.playing == 0 ? "Play Clip" : "Pause"), systemImage: m.status.playing == 0 ? "play.fill" : "pause.fill") }.disabled(m.status.duration < 0.1) }
            Picker("Source",selection:$m.sourceId) { ForEach(m.sources,id:\.id) { source in Text(cString(source.name)).tag(source.id) } }.disabled(m.capturing)
            Button("Refresh sources") { m.refreshSources() }
            if m.hasSource {
                HStack {
                    Button { m.transport(0) } label: { Image(systemName: "backward.fill") }.help("Previous track in \(m.capturedSourceName) · Control Option P")
                    Button { m.transport(1) } label: { Image(systemName: "playpause.fill") }.help("Play or pause \(m.capturedSourceName) · Control Option Space")
                    Button { m.transport(2) } label: { Image(systemName: "forward.fill") }.help("Next track in \(m.capturedSourceName) · Control Option N")
                }
            }
            Button(m.capturing ? "Stop Capture" : "Start Capture") { m.capturing ? m.stopCapture() : m.arm() }.disabled(!m.capturing && m.sourceId == 0).help(m.capturing ? "Stop capture and keep the captured audio available" : "Start capture and replace the current capture buffer")
            Button("Open Practice") { m.revealPractice() }
            Button("Live Sing…") { m.revealSing() }
            Divider(); Text("Recent practice").font(.caption.bold())
            ForEach(Array(m.history.enumerated()), id: \.offset) { _, h in Button(cString(h.title)) { ws_select(m.ws, h.id) }.buttonStyle(.plain) }
            Button("Quit Halftape") { NSApp.terminate(nil) }
        }.padding(16).frame(width: 270)
            .onAppear { m.setWindowOpeners(practice: { openWindow(id: "practice") }, sing: { openWindow(id: "sing") }) }
    }
}

struct PracticeView: View {
    @EnvironmentObject var m: PracticeModel
    @Environment(\.openWindow) private var openWindow
    @State private var speed = 1.0
    @State private var pitch = 0.0
    @State private var cents = 0.0
    @State private var draggingA = false
    @State private var draggingB = false
    @State private var gridOffset = 0.0
    @State private var trainerStart = 0.8
    @State private var trainerStep = 0.05
    @State private var trainerEvery = 4.0
    @State private var dragStartA = 0.0
    @State private var dragStartB = 0.0
    @State private var keyMonitor: Any?
    let names = ["Vocals", "Drums", "Bass", "Guitar", "Piano", "Other"]
    var body: some View {
        VStack(spacing: 0) {
            HStack { Text(m.isSinging ? "Live Sing · \(singStateLabel)" : cString(m.status.message)).font(.caption).foregroundStyle(.secondary).lineLimit(1); Spacer() }.padding(.horizontal,20).padding(.top,8)
            HStack(spacing: 0) {
                ScrollView {
                VStack(spacing: 14) {
                    captureSection.padding(.horizontal,20).padding(.top,12)
                    if m.isSinging {
                        liveSingControls.padding(.horizontal,20)
                    } else {
                        waveformView.frame(height: 205).padding(.horizontal, 20)
                        transport
                        HStack(spacing: 24) { parameter("Speed", value: $speed, range: 0.25...1.5, display: "\(Int(speed * 100))%", step: 0.05) { m.setSpeed(speed) }; parameter("Pitch", value: $pitch, range: -12...12, display: String(format: "%+.0f st", pitch), step: 1) { m.setPitch(pitch + cents / 100) } }.padding(.horizontal, 24)
                        HStack { Text("Fine tune").font(.caption).foregroundStyle(.secondary); Button("−") { cents=max(-100,cents-1); m.setPitch(pitch+cents/100) }; Slider(value:$cents,in:-100...100,step:1).tint(accent).onChange(of:cents) { _,_ in m.setPitch(pitch+cents/100) }; Text(String(format:"%+.0f¢",cents)).font(.system(.caption,design:.monospaced)); Button("+") { cents=min(100,cents+1); m.setPitch(pitch+cents/100) } }.padding(.horizontal,24)
                        stemMixer
                        trainerControls
                    }
                    Spacer(minLength: 0)
                }.frame(maxWidth: .infinity)
                }
                Rectangle().fill(Color(nsColor: .separatorColor)).frame(width: 1)
                sidebar.frame(width: 260)
            }
        }.background(canvas)
            .toolbar {
                ToolbarItemGroup(placement: .principal) {
                    Toggle("Pause source on Grab",isOn:$m.paused).onChange(of:m.paused) { _,v in ws_pause_source(m.ws,v ? 1:0) }
                    Button { m.chooser.toggle() } label: { Label("Grab length",systemImage:"timer") }
                        .popover(isPresented:$m.chooser,arrowEdge:.bottom) {
                            VStack(alignment:.leading,spacing:10) {
                                Text("Grab length: \(Int(m.grabSeconds)) seconds")
                                Slider(value:$m.grabSeconds,in:1...180,step:1)
                                Button("Grab") { m.grab(m.grabSeconds) }.disabled(!m.canGrab)
                            }.padding().frame(width:230)
                        }
                    Button("Demo") { ws_demo(m.ws) }.help("Generate a labeled demo clip for testing without system audio permission")
                }
            }
            .onAppear { installKeyboard(); m.refreshSources(); m.setWindowOpeners(practice: { openWindow(id: "practice") }, sing: { openWindow(id: "sing") }) }
            .onDisappear { if let keyMonitor { NSEvent.removeMonitor(keyMonitor); self.keyMonitor = nil } }
            .onChange(of:m.status.speed) { _,v in if v > 0 { speed=Double(v) } }
            .onChange(of:m.status.pitch) { _,v in
                let value = Double(v)
                pitch = min(12, max(-12, value.rounded()))
                cents = (value - pitch) * 100
            }
    }
    var captureSection: some View {
        VStack(alignment:.leading,spacing:9) {
            Text("Capture").font(.headline)
            HStack(spacing:10) {
                Picker("Source",selection:$m.sourceId) {
                    Text(m.sources.isEmpty ? "No audio sources" : "Choose source").tag(UInt32(0))
                    ForEach(m.sources,id:\.id) { source in Text(cString(source.name)).tag(source.id) }
                }.frame(maxWidth:260).disabled(m.capturing)
                Button("Refresh") { m.refreshSources() }
                Button(m.capturing ? "Stop Capture" : "Start Capture") { m.capturing ? m.stopCapture() : m.arm() }
                    .disabled(!m.capturing && m.sourceId == 0)
                    .help(m.capturing ? "Stop capture and keep the captured audio available" : "Start capture and replace the current capture buffer")
            }
            HStack { Stepper(value:$m.grabSeconds,in:1...180,step:1) { Text("Duration · \(Int(m.grabSeconds))s") }.frame(width:210); Button(m.grabLabel) { m.grab() }.buttonStyle(.borderedProminent).disabled(!m.canGrab) }
            HStack { Button("Live Sing…") { openWindow(id: "sing") }; if m.isSinging { Text(m.status.sing_state == 4 ? "Waiting for source audio" : singStateLabel).font(.caption).foregroundStyle(.secondary) } }
            Text(m.captureLabel).font(.caption).foregroundStyle(.secondary)
            Text("Play audio in the source, start capture, then grab a section. Capture keeps the latest three minutes.").font(.caption).foregroundStyle(.secondary)
        }.padding(12).background(Color(nsColor:.controlBackgroundColor),in:RoundedRectangle(cornerRadius:8))
    }
    var waveformView: some View {
        GeometryReader { geo in
            let w = geo.size.width, h = geo.size.height
            ZStack {
                RoundedRectangle(cornerRadius: 8).fill(Color(nsColor: .textBackgroundColor)).allowsHitTesting(false)
                Canvas { ctx, size in
                    for i in 0...8 { let x = size.width * CGFloat(i) / 8; var p = Path(); p.move(to: CGPoint(x:x,y:12)); p.addLine(to: CGPoint(x:x,y:size.height-12)); ctx.stroke(p, with: .color(Color(nsColor: .separatorColor).opacity(0.35)), lineWidth: 1) }
                    for beat in m.beats { let x = CGFloat(beat / max(0.1, m.status.duration)) * size.width; var p=Path(); p.move(to: CGPoint(x:x,y:8)); p.addLine(to: CGPoint(x:x,y:size.height-8)); ctx.stroke(p, with: .color(accent.opacity(0.18)), lineWidth: 1) }
                    if m.waveform.isEmpty { }
                    else { let stride = max(1,m.waveform.count/500); for i in Swift.stride(from: 0, to: m.waveform.count, by: stride) { let f=CGFloat(i)/CGFloat(m.waveform.count); let amp=CGFloat(abs(m.waveform[i]))*size.height*0.42; var p=Path(); p.move(to: CGPoint(x:f*size.width,y:size.height/2-amp)); p.addLine(to: CGPoint(x:f*size.width,y:size.height/2+amp)); ctx.stroke(p, with: .color(accent.opacity(0.75)), lineWidth: 1) } }
                }.allowsHitTesting(false)
                Color.clear.contentShape(Rectangle())
                    .gesture(SpatialTapGesture().onEnded { value in if w > 0 { m.seek(Double(value.location.x / w) * m.status.duration) } })
                    .simultaneousGesture(DragGesture(minimumDistance: 8).onEnded { value in if w > 0 { m.seek(Double(value.location.x / w) * m.status.duration) } })
                Rectangle().fill(Color(nsColor: .separatorColor)).frame(height: 1).offset(y: 0).allowsHitTesting(false)
                let a = CGFloat(m.status.loop_a / max(0.1,m.status.duration))*w, b = CGFloat(m.status.loop_b / max(0.1,m.status.duration))*w
                Rectangle().fill(accent.opacity(0.09)).frame(width: max(0,b-a), height: h).offset(x: (a+b)/2-w/2).allowsHitTesting(false)
                if m.waveform.isEmpty { Text("Grab a clip to see its waveform").font(.caption).foregroundStyle(.secondary).allowsHitTesting(false) }
                marker("A", x: a, width:w, height: h, active: draggingA).gesture(DragGesture().onChanged { v in if !draggingA { dragStartA=m.status.loop_a }; draggingA=true; m.loopA(max(0,dragStartA+Double(v.translation.width/w)*m.status.duration),m.status.loop_b) }.onEnded { _ in draggingA=false })
                marker("B", x: b, width:w, height: h, active: draggingB).gesture(DragGesture().onChanged { v in if !draggingB { dragStartB=m.status.loop_b }; draggingB=true; m.loopA(m.status.loop_a,max(0,dragStartB+Double(v.translation.width/w)*m.status.duration)) }.onEnded { _ in draggingB=false })
                Rectangle().fill(Color.primary.opacity(0.75)).frame(width: 1).offset(x: CGFloat(m.status.position/max(0.1,m.status.duration))*w-w/2).allowsHitTesting(false)
                VStack { HStack { Text("Loop region").font(.caption.weight(.semibold)); Spacer(); Text("\(time(m.status.duration))").font(.system(.caption,design:.monospaced)) }; Spacer(); HStack { Toggle("Snap to beat", isOn: $m.snap).controlSize(.small); Spacer(); Text("Click or drag to seek").font(.caption).foregroundStyle(.secondary).allowsHitTesting(false) } }.padding(12)
            }
        }
    }
    func marker(_ label:String,x:CGFloat,width:CGFloat,height:CGFloat,active:Bool)->some View { VStack(spacing:0) { Text(label).font(.caption2.weight(.semibold)).foregroundStyle(.white).frame(width:22,height:20).background(accent, in: RoundedRectangle(cornerRadius:4)); Rectangle().fill(accent).frame(width:2,height:height-42) }.position(x:min(max(12,x), max(12,width-12)),y:height/2) }
    var liveSingControls: some View { LiveSingControls(showsReturnToClip: true) }
    var singStateLabel: String {
        switch m.status.sing_state {
        case 1: return "Buffering source audio"
        case 2: return "Processing audio"
        case 3: return "Playing Sing audio"
        case 4: return "Waiting for source audio"
        default: return "Live Sing active"
        }
    }
    var transport: some View { VStack(spacing:8) {
        HStack(spacing:12) {
            Button { m.togglePlay() } label: { Label(m.status.playing == 0 ? "Play Clip" : "Pause", systemImage: m.status.playing == 0 ? "play.fill" : "pause.fill") }.buttonStyle(.borderedProminent).tint(accent).disabled(m.status.duration < 0.1)
            Button { m.seek(m.status.position - 5) } label: { Image(systemName:"gobackward.5") }.help("Back 5 seconds").disabled(m.status.duration < 0.1)
            Button { m.seek(m.status.position + 5) } label: { Image(systemName:"goforward.5") }.help("Forward 5 seconds").disabled(m.status.duration < 0.1)
            Text(String(format:"%.0f BPM",m.status.bpm)).font(.system(.caption,design:.monospaced))
        }
        HStack { Text(time(m.status.position)).font(.system(.caption,design:.monospaced)); Slider(value:Binding(get:{m.status.position},set:{m.seek($0)}),in:0...max(0.1,m.status.duration)).disabled(m.status.duration < 0.1); Text(time(m.status.duration)).font(.system(.caption,design:.monospaced)) }
        HStack { Toggle("Loop",isOn:Binding(get:{m.status.looping != 0},set:{m.loopEnabled($0)})).disabled(m.status.duration < 0.1); Button("A") { m.loopA(m.status.position,m.status.loop_b) }.disabled(m.status.duration < 0.1); Button("B") { m.loopA(m.status.loop_a,m.status.position) }.disabled(m.status.duration < 0.1); Toggle("Snap",isOn:$m.snap).controlSize(.small); Spacer(); Text("Grid offset"); Slider(value:$gridOffset,in:-0.5...0.5,step:0.01).frame(width:110).disabled(m.status.duration < 0.1).onChange(of:gridOffset) { _,v in ws_grid_offset(m.ws,v) } }.font(.caption)
    }.padding(.horizontal,22) }
    var stemMixer: some View { VStack(spacing:8) { HStack { Text("Stem levels").font(.headline); Spacer(); Button { openWindow(id: "sing") } label: { Label("Sing panel",systemImage:"mic") }.buttonStyle(.bordered) }
        if m.modelSix.isEmpty { Text("Choose a 6-stem separation model to enable stem controls").font(.caption).foregroundStyle(.secondary); Button("Choose Model") { m.modelPicker(six:true) } }
        if m.status.stems_ready > 0 && m.status.current_stems_ready == 0 { Text("This part of the clip still plays the original mix. Mute and Solo apply once its stems are ready.").font(.caption).foregroundStyle(.secondary) }
        if m.status.stems_ready > 0 { Text("Pause the source app to hear only this clip.").font(.caption).foregroundStyle(.secondary) }
        if m.status.busy != 0 { HStack { ProgressView(value:Double(m.status.progress)); Text("Separating · \(Int(m.status.progress*100))%").font(.caption).foregroundStyle(.secondary) } }
        else if m.status.stems_ready == 0 && m.status.busy == 0 && (!m.modelSix.isEmpty || !m.modelFour.isEmpty) { Text("Model selected · loads when separation begins").font(.caption).foregroundStyle(.secondary) }
        ForEach(0..<6,id:\.self) { i in StemRow(index:i,name:names[i]) }
    }.padding(.horizontal,24).padding(.top,5) }
    var trainerControls: some View { GroupBox("Loop trainer") { VStack(alignment:.leading,spacing:10) {
        HStack { Toggle("Enable trainer",isOn:$m.trainer).onChange(of:m.trainer) { _,_ in configureTrainer() }; Toggle("Count-in",isOn:$m.countIn).onChange(of:m.countIn) { _,v in ws_count_in(m.ws,v ? 1:0) } }
        Grid(alignment:.leading,horizontalSpacing:12,verticalSpacing:6) {
            GridRow { Text("Start speed"); Slider(value:$trainerStart,in:0.25...1,step:0.05); Text("\(Int(trainerStart*100))%") }
            GridRow { Text("Speed increase"); Slider(value:$trainerStep,in:0.01...0.2,step:0.01); Text("\(Int(trainerStep*100))%") }
            GridRow { Text("Increase every"); Stepper(value:$trainerEvery,in:1...16,step:1) { Text("\(Int(trainerEvery)) loops") }; Text("") }
        }
        HStack { Spacer(); Button("Start Trainer") { m.trainer=true; configureTrainer(); ws_play(m.ws,1) }.buttonStyle(.borderedProminent).tint(accent).disabled(m.status.duration < 0.1) }
    }.frame(maxWidth:.infinity,alignment:.leading) }.padding(.horizontal,20) }
    var sidebar: some View { ScrollView { VStack(alignment:.leading,spacing:14) {
        GroupBox("Session") { VStack(alignment:.leading,spacing:8) { metric("Capture", m.capturing ? "Capturing" : (m.status.buffered >= 0.1 ? "Stopped" : "Not capturing")); metric("Clip", "\(Int(m.status.duration))s"); metric("Loop", "\(time(m.status.loop_a)) – \(time(m.status.loop_b))"); metric("Key", keyName(m.status.key)); metric("Status", sessionStatus) }.frame(maxWidth:.infinity,alignment:.leading) }
        GroupBox("Recent clips") {
            VStack(alignment:.leading,spacing:8) {
                if m.history.isEmpty { ContentUnavailableView("No clips yet", systemImage:"waveform", description:Text("Choose a source and grab a moment to practice.")) }
                ForEach(Array(m.history.enumerated()),id:\.offset) { _,h in Button { ws_select(m.ws,h.id) } label: { Label { VStack(alignment:.leading) { Text(cString(h.title)).lineLimit(1); Text(String(format:"%.0f sec",h.duration)).font(.caption).foregroundStyle(.secondary) } } icon: { Image(systemName:"waveform") } }.buttonStyle(.plain) }
            }.frame(maxWidth:.infinity,alignment:.leading)
        }
        GroupBox("Models") {
            VStack(alignment:.leading,spacing:8) {
                Button { m.modelPicker(six:true) } label: { modelLabel("6-stem",path:m.modelSix) }.buttonStyle(.plain)
                Button { m.singModelPicker() } label: { modelLabel(m.singClean ? "Sing · UMX-L" : "Sing · StemGenRT",path:m.singClean ? m.modelClean : m.modelSing) }.buttonStyle(.plain).help(m.singClean ? "Choose the clean Sing model" : "Choose StemGenRT hop-128 ONNX model")
                Text("Live Sing streams vocals and accompaniment; separation may leave some vocals.").font(.caption).foregroundStyle(.secondary)
            }.frame(maxWidth:.infinity,alignment:.leading)
        }
        Spacer()
    }.padding(18).frame(maxWidth:.infinity,alignment:.topLeading) }.frame(maxHeight:.infinity) }
    func metric(_ title:String,_ value:String)->some View { HStack { Text(title).foregroundStyle(.secondary); Spacer(); Text(value).font(.system(size:11,weight:.medium,design:.monospaced)) }.font(.caption) }
    var sessionStatus:String { if m.isSinging { return "Live Sing · \(singStateLabel)" }; return m.status.busy != 0 ? "Processing" : (m.status.playing != 0 ? "Playing clip" : "Ready") }
    func modelLabel(_ title:String,path:String)->some View { HStack { Image(systemName:"shippingbox").foregroundStyle(accent); Text(title); Spacer(); Text(path.isEmpty ? "Choose ONNX / BIN" : URL(fileURLWithPath:path).lastPathComponent).font(.caption2).foregroundStyle(.secondary).lineLimit(1) }.help("ONNX preferred; BIN also supported") }
    func parameter(_ name:String,value:Binding<Double>,range:ClosedRange<Double>,display:String,step:Double,changed:@escaping()->Void)->some View { VStack(alignment:.leading,spacing:6) { HStack { Text(name).font(.caption).foregroundStyle(.secondary); Spacer(); Text(display).font(.system(.caption,design:.monospaced)) }; HStack(spacing:8) { Button("−") { value.wrappedValue=max(range.lowerBound,value.wrappedValue-step); changed() }.buttonStyle(.plain); Slider(value:value,in:range,onEditingChanged:{ _ in changed() }).tint(accent); Button("+") { value.wrappedValue=min(range.upperBound,value.wrappedValue+step); changed() }.buttonStyle(.plain) } }.frame(maxWidth:.infinity) }
    func configureTrainer() { ws_trainer(m.ws,m.trainer ? 1:0,Float(trainerStart),Float(trainerStep),Int32(trainerEvery)) }
    func nudgeEndpoint(_ sign:Double) { let delta=sign*60/max(1,m.status.bpm); if sign < 0 { m.loopA(max(0,m.status.loop_a+delta),m.status.loop_b) } else { m.loopA(m.status.loop_a,max(m.status.loop_a+0.01,m.status.loop_b+delta)) } }
    func time(_ s:Double)->String { String(format:"%02d:%02d",Int(s)/60,Int(s)%60) }
    func keyName(_ key:Int32)->String {
        let names=["C","C♯","D","E♭","E","F","F♯","G","A♭","A","B♭","B"]
        guard (0..<24).contains(key) else { return "Unknown" }
        return "\(names[Int(key % 12)]) \(key < 12 ? "major" : "minor")"
    }
    func installKeyboard() { keyMonitor = NSEvent.addLocalMonitorForEvents(matching:.keyDown) { event in
        guard NSApp.keyWindow?.title == "Halftape" else { return event }
        if !event.modifierFlags.intersection([.command, .control, .option]).isEmpty { return event }
        if NSApp.keyWindow?.firstResponder is NSTextView || NSApp.keyWindow?.firstResponder is NSTextField { return event }
        if event.keyCode == 49 { m.togglePlay(); return nil }
        if m.isSinging { return event }
        if event.keyCode == 123 { m.seek(m.status.position - 5); return nil }
        if event.keyCode == 124 { m.seek(m.status.position + 5); return nil }
        let key=event.charactersIgnoringModifiers ?? ""
        if key == "[" { guard m.status.duration >= 0.1 else { return nil }; nudgeEndpoint(-1); return nil }; if key == "]" { guard m.status.duration >= 0.1 else { return nil }; nudgeEndpoint(1); return nil }
        if key == "," { speed=max(0.25,speed-0.05); m.setSpeed(speed); return nil }; if key == "." { speed=min(1.5,speed+0.05); m.setSpeed(speed); return nil }
        if key == "-" { pitch=max(-12,pitch-1); m.setPitch(pitch+cents/100); return nil }; if key == "=" { pitch=min(12,pitch+1); m.setPitch(pitch+cents/100); return nil }
        let digitKeys: [Int: Int] = [18: 0, 19: 1, 20: 2, 21: 3, 23: 4, 22: 5]
        if let i = digitKeys[Int(event.keyCode)], m.status.stems_ready > 0 { if event.modifierFlags.contains(.shift) { m.mutes[i].toggle() } else { m.solos[i].toggle() }; m.updateStem(i); return nil }
        return event
    } }
}

struct StemRow: View {
    @EnvironmentObject var m:PracticeModel
    let index:Int; let name:String
    var body: some View { HStack(spacing:10) { Text(name).frame(width:60,alignment:.leading); if m.status.busy != 0 { ProgressView().controlSize(.small).frame(width:14) }; Slider(value:$m.gains[index],in:0...1).disabled(m.status.stems_ready == 0).onChange(of:m.gains[index]) { _,_ in m.updateStem(index) }; Toggle("Mute",isOn:$m.mutes[index]).toggleStyle(.checkbox).controlSize(.small).disabled(m.status.stems_ready == 0).help("Mute \(name)").onChange(of:m.mutes[index]) { _,_ in if m.status.stems_ready > 0 { m.updateStem(index) } }; Toggle("Solo",isOn:$m.solos[index]).toggleStyle(.checkbox).controlSize(.small).disabled(m.status.stems_ready == 0).help("Solo \(name)").onChange(of:m.solos[index]) { _,_ in if m.status.stems_ready > 0 { m.updateStem(index) } } }.frame(height:30) }
}

struct SingPanel: View {
    @EnvironmentObject var m:PracticeModel
    var body: some View { LiveSingControls(showsReturnToClip: true).background(FloatingLevelAccessor()) }
}

struct LiveSingControls: View {
    @EnvironmentObject var m:PracticeModel
    var showsReturnToClip: Bool
    var stateLabel: String {
        switch m.status.sing_state { case 1:return "Buffering input"; case 2:return "Processing"; case 3:return "Playing Sing audio"; case 4:return "Waiting for source audio"; default:return "Stopped" }
    }
    var body: some View { VStack(alignment:.leading,spacing:12) {
        HStack { Label("Live Sing",systemImage:"mic.fill").font(.headline); Spacer(); Text(stateLabel).font(.caption).foregroundStyle(.secondary) }
        Picker("Mode", selection: Binding(get: { m.singClean }, set: { m.setSingMode($0) })) {
            Text("Clean · about 0.25 s").tag(true)
            Text("Low latency").tag(false)
        }.pickerStyle(.segmented).disabled(m.status.sing != 0)
        if m.lowPowerMode {
            Label("Low Power Mode is on. Clean Sing may fall behind on some Macs. If it stutters, turn it off in System Settings under Battery, or use Low latency.", systemImage: "battery.25percent").font(.caption).foregroundStyle(.orange)
        }
        if m.status.sing == 0 {
            if m.status.sing_model_ready == 0 {
                Text(m.singClean ? "Run scripts/sing-clean-model.sh or choose umxl-vocals.onnx to enable clean Sing." : "Choose the StemGenRT hop-128 ONNX model to enable Live Sing.").font(.caption).foregroundStyle(.secondary)
                Button("Choose Sing Model…") { m.singModelPicker() }
            } else {
                Text(m.capturing ? "Play audio in \(m.capturedSourceName), then click Start Sing." : "Choose a source and click Start Capture in the main window before starting Sing.").font(.caption).foregroundStyle(.secondary)
            }
        } else {
            Text("Source: \(m.capturedSourceName)").font(.subheadline)
            if m.status.sing_state == 1 || m.status.sing_state == 2 {
                HStack { ProgressView().controlSize(.small); Text(stateLabel); Spacer(); Text(String(format:"%.0f ms buffered",m.status.sing_buffered * 1000)) }.font(.caption).foregroundStyle(.secondary)
            } else if m.status.sing_state == 4 {
                HStack { ProgressView().controlSize(.small); Text("Waiting for audio from \(m.capturedSourceName)…") }.font(.caption).foregroundStyle(.secondary)
            } else if m.status.sing_state == 3 {
                Label(String(format:"Delay · %.0f ms, of which %.0f ms is the audio device",m.status.sing_latency * 1000,m.status.device_latency * 1000),systemImage:"clock").font(.caption).foregroundStyle(.secondary)
                Text("Transpose can add a little extra delay.").font(.caption).foregroundStyle(.secondary)
                Text(String(format:"%.1f s played",m.status.sing_position)).font(.system(.caption,design:.monospaced)).foregroundStyle(.secondary)
            }
        }
        GroupBox("Vocal reduction · \(Int(m.status.vocal_removal * 100))%") {
            Slider(value:Binding(get:{Double(m.status.vocal_removal)},set:{ws_vocals(m.ws,Float($0)); m.poll()}),in:0...1).tint(accent).disabled(m.status.sing == 0)
        }
        GroupBox("Transpose") { HStack {
            Button("−") { ws_sing_pitch(m.ws,max(-6,m.status.sing_pitch-1)); m.poll() }.disabled(m.status.sing == 0 || m.status.sing_pitch <= -6)
            Text(String(format:"%+.0f semitones",Double(m.status.sing_pitch))).font(.system(.body,design:.monospaced))
            Button("+") { ws_sing_pitch(m.ws,min(6,m.status.sing_pitch+1)); m.poll() }.disabled(m.status.sing == 0 || m.status.sing_pitch >= 6)
        }.frame(maxWidth:.infinity) }
        GroupBox("Separation") { HStack { Text("Model"); Spacer(); Text(qualityName).foregroundStyle(.secondary) } }
        GroupBox("Lyrics mirror") { VStack(alignment: .leading, spacing: 8) {
            Toggle("Delay the lyrics on screen to match the audio", isOn: Binding(get: { m.mirror.enabled }, set: { m.mirror.enabled = $0 })).disabled(!m.hasSource)
            if m.mirror.enabled {
                let state = m.mirror.state
                let good = { if case .mirroring = state { return true } else { return false } }()
                let bad = { switch state { case .needsPermission, .failed: return true; default: return false } }()
                Label(state.text, systemImage: state.symbol).font(.caption).foregroundStyle(good ? .green : bad ? .orange : .secondary)
                HStack {
                    if m.mirror.settingUp {
                        Button("Cancel setup") { m.mirror.cancelSetup() }
                    } else {
                    switch state {
                    case .needsPermission:
                        Button("Open System Settings") { m.mirror.openPermissionSettings() }
                    case .noSource, .windowOffScreen:
                        EmptyView()
                    default:
                        if m.mirror.areaSet {
                            Button("Show area") { m.mirror.showArea() }
                            Button("Change…") { m.mirror.beginSetup() }
                            Button("Clear") { m.mirror.clearArea() }
                        } else {
                            Button("Set lyrics area…") { m.mirror.beginSetup() }
                        }
                    }
                    }
                }
                Toggle("Show the delay badge on the mirror", isOn: Binding(get: { m.mirror.showBadge }, set: { m.mirror.showBadge = $0 })).font(.caption)
            }
        } }
        Text("Streaming separation may leave some vocals.").font(.caption).foregroundStyle(.secondary)
        Text("Skip or pause from the menu bar, with Control Option P, Space and N, or in the source app. Use the clip for looping and individual stem controls.").font(.caption).foregroundStyle(.secondary)
        HStack { Button(m.status.sing == 0 ? "Start Sing" : "Stop Sing") { m.toggleSing() }.buttonStyle(.borderedProminent).disabled(m.status.sing == 0 && (!m.capturing || m.status.sing_model_ready == 0)); Spacer(); if showsReturnToClip { Button("Return to Clip") { ws_play(m.ws,1); m.poll() }.disabled(m.status.duration < 0.1) } }
    }.padding(16).frame(maxWidth:.infinity,alignment:.leading).background(Color(nsColor:.controlBackgroundColor),in:RoundedRectangle(cornerRadius:10)) }
    var qualityName:String { m.status.sing == 0 ? "Stopped" : (m.singClean ? "UMX-L (clean)" : "StemGenRT") }
}

private struct FloatingLevelAccessor: NSViewRepresentable {
    func makeNSView(context: Context) -> FloatingLevelView { FloatingLevelView(frame: .zero) }
    func updateNSView(_ view: FloatingLevelView, context: Context) { view.window?.level = .floating }
}

private final class FloatingLevelView: NSView {
    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        window?.level = .floating
    }
}
