import SwiftUI
import AppKit
import Carbon
import UniformTypeIdentifiers

@main
struct HalftapeApp: App {
    @StateObject private var model = PracticeModel()
    var body: some Scene {
        Window("Halftape", id: "practice") { PracticeView().environmentObject(model).frame(minWidth: 900, minHeight: 650) }
            .defaultSize(width: 1120, height: 780)
        MenuBarExtra("\(model.armedSourceName) · \(Int(model.status.buffered))s", systemImage: "waveform") { MenuContent().environmentObject(model) }
            .menuBarExtraStyle(.window)
        Window("Sing", id: "sing") { SingPanel().environmentObject(model).frame(width: 360, height: 310) }
            .defaultSize(width: 360, height: 310)
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
    @Published var sources: [WSSource] = []
    var armedSourceName: String { sources.first(where: { $0.id == sourceId }).map { cString($0.name) } ?? "Halftape" }
    @Published var history: [WSHistory] = []
    @Published var waveform: [Float] = []
    @Published var beats: [Double] = []
    @Published var sourceId: UInt32 = 0
    @Published var snap = true
    @Published var paused = false
    @Published var trainer = false
    @Published var countIn = true
    @Published var chooser = false
    @Published var grabSeconds = 30.0
    @Published var heldGrab = false
    @Published var modelSix = UserDefaults.standard.string(forKey: "modelSix") ?? ""
    @Published var modelFour = UserDefaults.standard.string(forKey: "modelFour") ?? ""
    @Published var gains = Array(repeating: 1.0, count: 6)
    @Published var mutes = Array(repeating: false, count: 6)
    @Published var solos = Array(repeating: false, count: 6)
    private var poller: Timer?
    private var holdTimer: Timer?
    private var lastGrab = Date.distantPast
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
        setModels()
        refreshSources()
        if sourceId != 0 { arm() }
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
    func poll() {
        ws_status(ws, &status)
        var wave = Array(repeating: Float(0), count: 1024)
        let wn = ws_waveform(ws, &wave, Int32(wave.count)); waveform = Array(wave.prefix(max(0, Int(wn))))
        var grid = Array(repeating: Double(0), count: 512)
        let bn = ws_beats(ws, &grid, Int32(grid.count)); beats = Array(grid.prefix(max(0, Int(bn))))
        var rows = Array(repeating: WSHistory(), count: 20)
        let hn = ws_history(ws, &rows, Int32(rows.count)); history = Array(rows.prefix(max(0, Int(hn))))
    }
    func arm() { ws_arm(ws, sourceId) }
    func updateStem(_ index:Int) { ws_stem(ws, Int32(index), Float(gains[index]), mutes[index] ? 1:0, solos[index] ? 1:0) }
    func grab(_ seconds: Double? = nil) {
        let now = Date()
        let next = seconds ?? (now.timeIntervalSince(lastGrab) < 1 ? min(180, grabSeconds + 30) : grabSeconds)
        grabSeconds = next; lastGrab = now; ws_grab(ws, next); chooser = false
    }
    func togglePlay() { ws_play(ws, status.playing == 0 ? 1 : 0) }
    func setSpeed(_ value: Double) { ws_speed(ws, Float(min(1.5, max(0.25, value)))) }
    func setPitch(_ value: Double) { ws_pitch(ws, Float(min(13, max(-13, value)))) }
    func loopA(_ a: Double, _ b: Double) { ws_loop(ws, a, b, snap ? 1 : 0) }
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
    func setModels() { modelSix.withCString { a in modelFour.withCString { b in ws_set_model(ws, a, b) } } }
    func startHold() { heldGrab = true; holdTimer?.invalidate(); holdTimer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: false) { [weak self] _ in guard let model = self else { return }; MainActor.assumeIsolated { if model.heldGrab { model.chooser = true } } } }
    func endHold() { let wasHeld = heldGrab; heldGrab = false; holdTimer?.invalidate(); if wasHeld && !chooser { grab() } }
}

@MainActor private final class HotKeyController {
    private weak var model: PracticeModel?
    private var gHotKey: EventHotKeyRef?
    private var kHotKey: EventHotKeyRef?
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
        }
    }
    func event(_ id: UInt32, pressed: Bool) {
        if id == 1 { if pressed, let m = model { m.revealPractice(); m.startHold() } else { model?.endHold() } }
        else if id == 2, pressed, let m = model { ws_sing(m.ws, m.status.sing == 0 ? 1 : 0); m.revealSing() }
    }
    fileprivate func dispose() {
        if let gHotKey { UnregisterEventHotKey(gHotKey) }; if let kHotKey { UnregisterEventHotKey(kHotKey) }
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
            HStack { Text("Halftape").font(.headline); Spacer(); Circle().fill(m.status.busy != 0 ? accent : .green).frame(width: 8,height: 8) }
            Text("Buffer · \(Int(m.status.buffered)) seconds").font(.caption).foregroundStyle(.secondary)
            Text(cString(m.status.message)).font(.caption2).foregroundStyle(.secondary).lineLimit(2)
            HStack { Button { m.revealPractice() } label: { Label("Grab \(Int(m.grabSeconds))s", systemImage: "arrow.down.to.line") }.keyboardShortcut("g", modifiers: [.control,.option]); Button { m.togglePlay() } label: { Label(m.status.playing == 0 ? "Play" : "Pause", systemImage: m.status.playing == 0 ? "play.fill" : "pause.fill") } }
            Button("Open Practice") { m.revealPractice() }
            Divider(); Text("Recent practice").font(.caption.bold())
            ForEach(Array(m.history.enumerated()), id: \.offset) { _, h in Button(cString(h.title)) { ws_select(m.ws, h.id) }.buttonStyle(.plain) }
            Button("Refresh sources") { m.refreshSources() }
            Button("Disarm source") { ws_disarm(m.ws) }
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
    @State private var removal = 0.7
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
            HStack { Text(cString(m.status.message)).font(.caption).foregroundStyle(.secondary).lineLimit(1); Spacer() }.padding(.horizontal,20).padding(.top,8)
            HStack(spacing: 0) {
                ScrollView {
                VStack(spacing: 14) {
                    waveformView.frame(height: 205).padding(.horizontal, 20).padding(.top, 18)
                    transport
                    HStack(spacing: 24) { parameter("Speed", value: $speed, range: 0.25...1.5, display: "\(Int(speed * 100))%", step: 0.05) { m.setSpeed(speed) }; parameter("Pitch", value: $pitch, range: -12...12, display: String(format: "%+.0f st", pitch), step: 1) { m.setPitch(pitch + cents / 100) } }.padding(.horizontal, 24)
                    HStack { Text("Fine tune").font(.caption).foregroundStyle(.secondary); Button("−") { cents=max(-100,cents-1); m.setPitch(pitch+cents/100) }; Slider(value:$cents,in:-100...100,step:1).tint(accent).onChange(of:cents) { _,_ in m.setPitch(pitch+cents/100) }; Text(String(format:"%+.0f¢",cents)).font(.system(.caption,design:.monospaced)); Button("+") { cents=min(100,cents+1); m.setPitch(pitch+cents/100) } }.padding(.horizontal,24)
                    stemMixer
                    trainerControls
                    Spacer(minLength: 0)
                }.frame(maxWidth: .infinity)
                }
                Rectangle().fill(Color(nsColor: .separatorColor)).frame(width: 1)
                sidebar.frame(width: 260)
            }
        }.background(canvas)
            .toolbar {
                ToolbarItemGroup(placement: .principal) {
                    Picker("Audio source", selection: $m.sourceId) {
                        Text(m.sources.isEmpty ? "No audio sources" : "Choose source").tag(UInt32(0))
                        ForEach(m.sources, id: \.id) { source in Text(cString(source.name)).tag(source.id) }
                    }
                        .frame(width: 220).onChange(of: m.sourceId) { _, _ in m.arm() }
                    Button { m.paused.toggle(); ws_pause_source(m.ws, m.paused ? 1 : 0) } label: { Label(m.paused ? "Paused on grab" : "Pause on grab", systemImage: m.paused ? "pause.circle.fill" : "waveform.circle") }
                    Button { m.grab() } label: { Label("Grab", systemImage: "arrow.down.to.line") }.buttonStyle(.borderedProminent).tint(accent)
                        .keyboardShortcut("g", modifiers: [.control,.option])
                    Button { m.chooser.toggle() } label: { Label("Grab length",systemImage:"timer") }
                        .popover(isPresented:$m.chooser,arrowEdge:.bottom) {
                            VStack(alignment:.leading,spacing:10) {
                                Text("Grab length: \(Int(m.grabSeconds)) seconds")
                                Slider(value:$m.grabSeconds,in:1...180,step:1)
                                Button("Grab") { m.grab(m.grabSeconds) }
                            }.padding().frame(width:230)
                        }
                    Button("Demo") { ws_demo(m.ws) }.help("Generate a labeled demo clip for testing without system audio permission")
                }
            }
            .onAppear { installKeyboard(); m.refreshSources(); if m.sourceId != 0 { m.arm() }; m.setWindowOpeners(practice: { openWindow(id: "practice") }, sing: { openWindow(id: "sing") }) }
            .onDisappear { if let keyMonitor { NSEvent.removeMonitor(keyMonitor); self.keyMonitor = nil } }
            .onChange(of:m.status.speed) { _,v in if v > 0 { speed=Double(v) } }
            .onChange(of:m.status.pitch) { _,v in
                let value = Double(v)
                pitch = min(12, max(-12, value.rounded()))
                cents = (value - pitch) * 100
            }
    }
    var waveformView: some View {
        GeometryReader { geo in
            let w = geo.size.width, h = geo.size.height
            ZStack {
                RoundedRectangle(cornerRadius: 8).fill(Color(nsColor: .textBackgroundColor))
                Canvas { ctx, size in
                    for i in 0...8 { let x = size.width * CGFloat(i) / 8; var p = Path(); p.move(to: CGPoint(x:x,y:12)); p.addLine(to: CGPoint(x:x,y:size.height-12)); ctx.stroke(p, with: .color(Color(nsColor: .separatorColor).opacity(0.35)), lineWidth: 1) }
                    for beat in m.beats { let x = CGFloat(beat / max(0.1, m.status.duration)) * size.width; var p=Path(); p.move(to: CGPoint(x:x,y:8)); p.addLine(to: CGPoint(x:x,y:size.height-8)); ctx.stroke(p, with: .color(accent.opacity(0.18)), lineWidth: 1) }
                    if m.waveform.isEmpty { }
                    else { let stride = max(1,m.waveform.count/500); for i in Swift.stride(from: 0, to: m.waveform.count, by: stride) { let f=CGFloat(i)/CGFloat(m.waveform.count); let amp=CGFloat(abs(m.waveform[i]))*size.height*0.42; var p=Path(); p.move(to: CGPoint(x:f*size.width,y:size.height/2-amp)); p.addLine(to: CGPoint(x:f*size.width,y:size.height/2+amp)); ctx.stroke(p, with: .color(accent.opacity(0.75)), lineWidth: 1) } }
                }
                Rectangle().fill(Color(nsColor: .separatorColor)).frame(height: 1).offset(y: 0)
                let a = CGFloat(m.status.loop_a / max(0.1,m.status.duration))*w, b = CGFloat(m.status.loop_b / max(0.1,m.status.duration))*w
                Rectangle().fill(accent.opacity(0.09)).frame(width: max(0,b-a), height: h).offset(x: (a+b)/2-w/2)
                if m.waveform.isEmpty { Text("Grab a clip to see its waveform").font(.caption).foregroundStyle(.secondary) }
                marker("A", x: a, width:w, height: h, active: draggingA).gesture(DragGesture().onChanged { v in if !draggingA { dragStartA=m.status.loop_a }; draggingA=true; m.loopA(max(0,dragStartA+Double(v.translation.width/w)*m.status.duration),m.status.loop_b) }.onEnded { _ in draggingA=false })
                marker("B", x: b, width:w, height: h, active: draggingB).gesture(DragGesture().onChanged { v in if !draggingB { dragStartB=m.status.loop_b }; draggingB=true; m.loopA(m.status.loop_a,max(0,dragStartB+Double(v.translation.width/w)*m.status.duration)) }.onEnded { _ in draggingB=false })
                Rectangle().fill(Color.primary.opacity(0.75)).frame(width: 1).offset(x: CGFloat(m.status.position/max(0.1,m.status.duration))*w-w/2)
                VStack { HStack { Text("Loop region").font(.caption.weight(.semibold)); Spacer(); Text("\(time(m.status.duration))").font(.system(.caption,design:.monospaced)) }; Spacer(); HStack { Toggle("Snap to beat", isOn: $m.snap).controlSize(.small); Spacer(); Text("Drag to align grid").font(.caption).foregroundStyle(.secondary) } }.padding(12)
            }.contentShape(Rectangle()).gesture(DragGesture(minimumDistance: 3).onChanged { v in gridOffset = v.translation.width / max(1,w) * m.status.duration; ws_grid_offset(m.ws, gridOffset) })
        }
    }
    func marker(_ label:String,x:CGFloat,width:CGFloat,height:CGFloat,active:Bool)->some View { VStack(spacing:0) { Text(label).font(.caption2.weight(.semibold)).foregroundStyle(.white).frame(width:22,height:20).background(accent, in: RoundedRectangle(cornerRadius:4)); Rectangle().fill(accent).frame(width:2,height:height-42) }.position(x:min(max(12,x), max(12,width-12)),y:height/2) }
    var transport: some View { HStack(spacing: 12) {
        Button { m.togglePlay() } label: { Label(m.status.playing == 0 ? "Play" : "Pause", systemImage: m.status.playing == 0 ? "play.fill" : "pause.fill") }.buttonStyle(.borderedProminent).tint(accent)
        Button { nudgeEndpoint(-1) } label: { Label("Previous beat", systemImage:"backward.end") }.labelStyle(.iconOnly).help("Move loop start back one beat")
        Button { nudgeEndpoint(1) } label: { Label("Next beat", systemImage:"forward.end") }.labelStyle(.iconOnly).help("Move loop end forward one beat")
        VStack(alignment:.leading,spacing:5) { ProgressView(value: max(0,min(1,m.status.progress))); HStack { Text(time(m.status.position)); Spacer(); Text(time(m.status.duration)) }.font(.system(.caption,design:.monospaced)).foregroundStyle(.secondary) }.frame(maxWidth:.infinity)
        Text(String(format:"%.0f BPM",m.status.bpm)).font(.system(.caption,design:.monospaced))
    }.padding(.horizontal,22) }
    var stemMixer: some View { VStack(spacing:8) { HStack { Text("Stem levels").font(.headline); Spacer(); Button { openWindow(id: "sing") } label: { Label("Sing panel",systemImage:"mic") }.buttonStyle(.bordered) }
        ForEach(0..<6,id:\.self) { i in StemRow(index:i,name:names[i]) }
    }.padding(.horizontal,24).padding(.top,5) }
    var trainerControls: some View { GroupBox("Loop trainer") { VStack(alignment:.leading,spacing:10) {
        HStack { Toggle("Enable trainer",isOn:$m.trainer).onChange(of:m.trainer) { _,_ in configureTrainer() }; Toggle("Count-in",isOn:$m.countIn).onChange(of:m.countIn) { _,v in ws_count_in(m.ws,v ? 1:0) } }
        Grid(alignment:.leading,horizontalSpacing:12,verticalSpacing:6) {
            GridRow { Text("Start speed"); Slider(value:$trainerStart,in:0.25...1,step:0.05); Text("\(Int(trainerStart*100))%") }
            GridRow { Text("Speed increase"); Slider(value:$trainerStep,in:0.01...0.2,step:0.01); Text("\(Int(trainerStep*100))%") }
            GridRow { Text("Increase every"); Stepper(value:$trainerEvery,in:1...16,step:1) { Text("\(Int(trainerEvery)) loops") }; Text("") }
        }
        HStack { Spacer(); Button("Start Trainer") { m.trainer=true; configureTrainer(); ws_play(m.ws,1) }.buttonStyle(.borderedProminent).tint(accent) }
    }.frame(maxWidth:.infinity,alignment:.leading) }.padding(.horizontal,20) }
    var sidebar: some View { ScrollView { VStack(alignment:.leading,spacing:14) {
        GroupBox("Session") { VStack(alignment:.leading,spacing:8) { metric("Buffer", "\(Int(m.status.buffered))s / \(Int(m.status.duration))s"); metric("Loop", "\(time(m.status.loop_a)) – \(time(m.status.loop_b))"); metric("Key", keyName(m.status.key)); metric("Status", m.status.busy != 0 ? "Processing" : (m.status.playing != 0 ? "Playing" : "Ready")) }.frame(maxWidth:.infinity,alignment:.leading) }
        GroupBox("Recent clips") {
            VStack(alignment:.leading,spacing:8) {
                if m.history.isEmpty { ContentUnavailableView("No clips yet", systemImage:"waveform", description:Text("Choose a source and grab a moment to practice.")) }
                ForEach(Array(m.history.enumerated()),id:\.offset) { _,h in Button { ws_select(m.ws,h.id) } label: { Label { VStack(alignment:.leading) { Text(cString(h.title)).lineLimit(1); Text(String(format:"%.0f sec",h.duration)).font(.caption).foregroundStyle(.secondary) } } icon: { Image(systemName:"waveform") } }.buttonStyle(.plain) }
            }.frame(maxWidth:.infinity,alignment:.leading)
        }
        GroupBox("Models") {
            VStack(alignment:.leading,spacing:8) {
                Button { m.modelPicker(six:true) } label: { modelLabel("6-stem",path:m.modelSix) }.buttonStyle(.plain)
                Button { m.modelPicker(six:false) } label: { modelLabel("4-stem",path:m.modelFour) }.buttonStyle(.plain)
                Text("ONNX preferred; BIN also supported").font(.caption).foregroundStyle(.secondary)
            }.frame(maxWidth:.infinity,alignment:.leading)
        }
        Spacer()
    }.padding(18).frame(maxWidth:.infinity,alignment:.topLeading) }.frame(maxHeight:.infinity) }
    func metric(_ title:String,_ value:String)->some View { HStack { Text(title).foregroundStyle(.secondary); Spacer(); Text(value).font(.system(size:11,weight:.medium,design:.monospaced)) }.font(.caption) }
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
        let key=event.charactersIgnoringModifiers ?? ""
        if key == "[" { nudgeEndpoint(-1); return nil }; if key == "]" { nudgeEndpoint(1); return nil }
        if key == "," { speed=max(0.25,speed-0.05); m.setSpeed(speed); return nil }; if key == "." { speed=min(1.5,speed+0.05); m.setSpeed(speed); return nil }
        if key == "-" { pitch=max(-12,pitch-1); m.setPitch(pitch+cents/100); return nil }; if key == "=" { pitch=min(12,pitch+1); m.setPitch(pitch+cents/100); return nil }
        let digitKeys: [Int: Int] = [18: 0, 19: 1, 20: 2, 21: 3, 23: 4, 22: 5]
        if let i = digitKeys[Int(event.keyCode)] { if event.modifierFlags.contains(.shift) { m.mutes[i].toggle() } else { m.solos[i].toggle() }; m.updateStem(i); return nil }
        return event
    } }
}

struct StemRow: View {
    @EnvironmentObject var m:PracticeModel
    let index:Int; let name:String
    var body: some View { HStack(spacing:10) { Text(name).frame(width:60,alignment:.leading); if m.status.busy != 0 { ProgressView().controlSize(.small).frame(width:14) }; Slider(value:$m.gains[index],in:0...1).onChange(of:m.gains[index]) { _,_ in m.updateStem(index) }; Toggle("Mute",isOn:$m.mutes[index]).toggleStyle(.checkbox).controlSize(.small).help("Mute \(name)").onChange(of:m.mutes[index]) { _,_ in m.updateStem(index) }; Toggle("Solo",isOn:$m.solos[index]).toggleStyle(.checkbox).controlSize(.small).help("Solo \(name)").onChange(of:m.solos[index]) { _,_ in m.updateStem(index) } }.frame(height:30) }
}

struct SingPanel: View {
    @EnvironmentObject var m:PracticeModel
    @State private var removal=0.7; @State private var transpose=0.0
    var body: some View { VStack(alignment:.leading,spacing:12) {
        HStack { Text("Sing Along").font(.headline); Spacer(); Toggle("Enabled",isOn:Binding(get:{m.status.sing != 0},set:{ws_sing(m.ws,$0 ? 1:0)})) }
        GroupBox("Vocal removal") { Slider(value:$removal,in:0...1).tint(accent).onChange(of:removal) { _,v in ws_vocals(m.ws,Float(v)) } }
        GroupBox("Transpose") { HStack { Button("−") { transpose=max(-6,transpose-1) }; Text(String(format:"%+.0f semitones",transpose)).font(.system(.body,design:.monospaced)); Button("+") { transpose=min(6,transpose+1) } }.onChange(of:transpose) { _,v in ws_pitch(m.ws,Float(v)) } }
        GroupBox("Separation") { HStack { Text("Automatic"); Spacer(); Text(["6-stem", "4-stem", "Center"].indices.contains(Int(m.status.quality)) ? ["6-stem", "4-stem", "Center"][Int(m.status.quality)] : "Fallback pending").foregroundStyle(.secondary) } }
        Label(String(format:"Delayed audio · %.1f s latency",m.status.sing_latency),systemImage:"clock").foregroundStyle(.secondary)
        Text(cString(m.status.message)).font(.caption).foregroundStyle(.secondary).lineLimit(2)
    }.padding(16).frame(maxWidth:.infinity,alignment:.leading).background(FloatingLevelAccessor()).onAppear { removal = Double(m.status.vocal_removal) } }
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
