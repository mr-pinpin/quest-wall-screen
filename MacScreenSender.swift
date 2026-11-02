import Cocoa
import CoreGraphics
import CoreMedia
import CoreVideo
import Network
import ScreenCaptureKit
import VideoToolbox

// Encoded video stays in memory. A separate mutually authenticated TLS relay
// exposes this loopback-only stream to the paired home Mini.
private extension Data {
    mutating func u16(_ value: UInt16) { var n=value.bigEndian; Swift.withUnsafeBytes(of:&n){append(contentsOf:$0)} }
    mutating func u32(_ value: UInt32) { var n=value.bigEndian; Swift.withUnsafeBytes(of:&n){append(contentsOf:$0)} }
    mutating func u64(_ value: UInt64) { var n=value.bigEndian; Swift.withUnsafeBytes(of:&n){append(contentsOf:$0)} }
}

private final class Peer {
    let connection: NWConnection
    var pending=0
    var needsKey=true
    init(_ c:NWConnection){connection=c}
}

private let compressionOutput: VTCompressionOutputCallback = { context, _, status, _, sample in
    guard status == noErr, let context=context, let sample=sample else { return }
    Unmanaged<ScreenSender>.fromOpaque(context).takeUnretainedValue().encoded(sample)
}

final class ScreenSender: NSObject, NSApplicationDelegate, SCStreamOutput, SCStreamDelegate {
    private var listener:NWListener?
    private var peers:[UUID:Peer]=[:]
    private let networkQueue=DispatchQueue(label:"com.pinpin.wall-screen.network")
    private let captureQueue=DispatchQueue(label:"com.pinpin.wall-screen.capture")
    private let stateLock=NSLock()
    private var forceKey=true
    private var firstPTS:Double?
    private var codecBytes:[UInt8]=[0x42,0,0x28]
    private var encoder:VTCompressionSession?
    private var stream:SCStream?
    private var starting=false
    private var enabled=true
    private var clientCount=0
    private var width=1920, height=1080
    private var statusItem:NSStatusItem!
    private let statusLine=NSMenuItem(title:"Starting…",action:nil,keyEquivalent:"")

    func applicationDidFinishLaunching(_ notification:Notification){
        statusItem=NSStatusBar.system.statusItem(withLength:NSStatusItem.variableLength)
        statusItem.button?.title="Wall Screen"
        let menu=NSMenu();statusLine.isEnabled=false;menu.addItem(statusLine)
        menu.addItem(NSMenuItem(title:"Share Mac screen",action:#selector(share),keyEquivalent:""))
        menu.addItem(NSMenuItem(title:"Stop sharing",action:#selector(stopSharing),keyEquivalent:""))
        menu.addItem(.separator());menu.addItem(NSMenuItem(title:"Quit",action:#selector(quit),keyEquivalent:"q"))
        for item in menu.items { item.target=self };statusItem.menu=menu
        do {
            let parameters=NWParameters.tcp
            parameters.requiredLocalEndpoint = .hostPort(host:"127.0.0.1",port:9234)
            let l=try NWListener(using:parameters)
            l.newConnectionHandler={ [weak self] c in self?.accept(c) }
            l.stateUpdateHandler={ [weak self] state in
                if case .ready=state { self?.status("Ready for your headset") }
                if case .failed(let e)=state { self?.status("Connection unavailable: \(e)") }
            }
            listener=l;l.start(queue:networkQueue)
        } catch { status("Cannot start sender: \(error)") }
        if CommandLine.arguments.contains("--request-permission") { share() }
    }

    private func status(_ text:String){ DispatchQueue.main.async { self.statusLine.title=text;print(text) } }
    @objc private func share(){
        enabled=true
        guard CGPreflightScreenCaptureAccess() else {
            status("Screen Recording permission needed")
            _=CGRequestScreenCaptureAccess()
            return
        }
        if clientCount>0 { startCapture() } else { status("Ready for your headset") }
    }
    @objc private func stopSharing(){
        enabled=false
        networkQueue.async { for p in self.peers.values { p.connection.cancel() };self.peers.removeAll() }
        stopCapture();status("Sharing stopped")
    }
    @objc private func quit(){stopSharing();NSApplication.shared.terminate(nil)}

    private func accept(_ c:NWConnection){
        let id=UUID();let peer=Peer(c);peers[id]=peer
        stateLock.lock();forceKey=true;stateLock.unlock()
        c.stateUpdateHandler={ [weak self] state in
            guard let self=self else{return}
            if case .ready=state {
                DispatchQueue.main.async { self.clientCount=self.peers.count;self.startCapture() }
                self.watch(id)
            }
            if case .failed=state { self.remove(id) }
            if case .cancelled=state { self.remove(id) }
        }
        c.start(queue:networkQueue)
    }
    private func watch(_ id:UUID){
        guard let p=peers[id] else{return}
        p.connection.receive(minimumIncompleteLength:1,maximumLength:1024){[weak self] _,_,done,error in
            guard let self=self else{return}
            if done || error != nil {self.remove(id)} else {self.watch(id)}
        }
    }
    private func remove(_ id:UUID){
        guard let peer=peers.removeValue(forKey:id) else{return};peer.connection.cancel()
        let count=peers.count
        DispatchQueue.main.async {self.clientCount=count;if count==0 {self.stopCapture();self.status("Ready for your headset")}}
    }
    private func sendError(_ message:String){
        var packet=Data([0x80]);packet.append(Data(message.utf8));broadcast(packet,key:true)
    }
    private func broadcast(_ packet:Data,key:Bool){
        networkQueue.async {
            var framed=Data();framed.u32(UInt32(packet.count));framed.append(packet)
            for peer in self.peers.values {
                if peer.pending>=2 {peer.needsKey=true;self.stateLock.lock();self.forceKey=true;self.stateLock.unlock();continue}
                if peer.needsKey && !key {continue}
                if key {peer.needsKey=false};peer.pending+=1
                peer.connection.send(content:framed,completion:.contentProcessed{error in
                    peer.pending=max(0,peer.pending-1)
                    if error != nil {peer.connection.cancel()}
                })
            }
        }
    }

    private func startCapture(){
        guard enabled,clientCount>0,!starting,stream==nil else{return}
        guard CGPreflightScreenCaptureAccess() else {
            status("Screen Recording permission needed");sendError("Allow Screen Recording for Wall Screen on your Mac.");return
        }
        starting=true
        Task {
            do {
                let content=try await SCShareableContent.excludingDesktopWindows(false,onScreenWindowsOnly:true)
                guard let display=content.displays.first(where:{$0.displayID==CGMainDisplayID()}) ?? content.displays.first else {
                    throw NSError(domain:"WallScreen",code:1,userInfo:[NSLocalizedDescriptionKey:"No display available"])
                }
                width=min(1920,display.width)/2*2;height=max(2,Int(Double(display.height)*Double(width)/Double(display.width))/2*2)
                var compression:VTCompressionSession?
                let result=VTCompressionSessionCreate(allocator:kCFAllocatorDefault,width:Int32(width),height:Int32(height),codecType:kCMVideoCodecType_H264,encoderSpecification:[kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder:true] as CFDictionary,imageBufferAttributes:nil,compressedDataAllocator:nil,outputCallback:compressionOutput,refcon:Unmanaged.passUnretained(self).toOpaque(),compressionSessionOut:&compression)
                guard result==noErr,let compression=compression else {throw NSError(domain:NSOSStatusErrorDomain,code:Int(result))}
                encoder=compression
                VTSessionSetProperty(compression,key:kVTCompressionPropertyKey_RealTime,value:kCFBooleanTrue)
                VTSessionSetProperty(compression,key:kVTCompressionPropertyKey_AllowFrameReordering,value:kCFBooleanFalse)
                VTSessionSetProperty(compression,key:kVTCompressionPropertyKey_ProfileLevel,value:kVTProfileLevel_H264_Baseline_AutoLevel)
                VTSessionSetProperty(compression,key:kVTCompressionPropertyKey_AverageBitRate,value:6_000_000 as CFNumber)
                VTSessionSetProperty(compression,key:kVTCompressionPropertyKey_ExpectedFrameRate,value:30 as CFNumber)
                VTSessionSetProperty(compression,key:kVTCompressionPropertyKey_MaxKeyFrameInterval,value:30 as CFNumber)
                VTCompressionSessionPrepareToEncodeFrames(compression)
                let configuration=SCStreamConfiguration();configuration.width=width;configuration.height=height
                configuration.minimumFrameInterval=CMTime(value:1,timescale:30);configuration.queueDepth=3
                configuration.pixelFormat=kCVPixelFormatType_32BGRA;configuration.showsCursor=true;configuration.capturesAudio=false
                let filter=SCContentFilter(display:display,excludingWindows:[])
                let capture=SCStream(filter:filter,configuration:configuration,delegate:self)
                try capture.addStreamOutput(self,type:.screen,sampleHandlerQueue:captureQueue)
                stream=capture;try await capture.startCapture();starting=false;status("Sharing your Mac screen")
            } catch {starting=false;stream=nil;status("Capture unavailable: \(error.localizedDescription)");sendError(error.localizedDescription)}
        }
    }
    private func stopCapture(){
        if let capture=stream {stream=nil;Task{try? await capture.stopCapture()}}
        if let compression=encoder {VTCompressionSessionCompleteFrames(compression,untilPresentationTimeStamp:.invalid);VTCompressionSessionInvalidate(compression);encoder=nil}
        firstPTS=nil
    }
    func stream(_ stream:SCStream,didStopWithError error:Error){status("Capture stopped: \(error.localizedDescription)");self.stream=nil}
    func stream(_ stream:SCStream,didOutputSampleBuffer sample:CMSampleBuffer,of type:SCStreamOutputType){
        guard type == .screen,CMSampleBufferDataIsReady(sample),let pixel=CMSampleBufferGetImageBuffer(sample),let compression=encoder else{return}
        stateLock.lock();let key=forceKey;forceKey=false;stateLock.unlock()
        let properties:CFDictionary?=key ? [kVTEncodeFrameOptionKey_ForceKeyFrame:true] as CFDictionary:nil
        VTCompressionSessionEncodeFrame(compression,imageBuffer:pixel,presentationTimeStamp:CMSampleBufferGetPresentationTimeStamp(sample),duration:CMTime(value:1,timescale:30),frameProperties:properties,sourceFrameRefcon:nil,infoFlagsOut:nil)
    }
    fileprivate func encoded(_ sample:CMSampleBuffer){
        guard let block=CMSampleBufferGetDataBuffer(sample),let format=CMSampleBufferGetFormatDescription(sample) else{return}
        let attachments=CMSampleBufferGetSampleAttachmentsArray(sample,createIfNecessary:false) as? [[String:Any]]
        let key = !(attachments?.first?[kCMSampleAttachmentKey_NotSync as String] as? Bool ?? false)
        var annex=Data()
        if key {
            for index in 0...1 {
                var pointer:UnsafePointer<UInt8>?;var length=0;var count=0;var header:Int32=0
                if CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format,parameterSetIndex:index,parameterSetPointerOut:&pointer,parameterSetSizeOut:&length,parameterSetCountOut:&count,nalUnitHeaderLengthOut:&header)==noErr,let pointer=pointer {
                    if index==0 && length>=4 {codecBytes=[pointer[1],pointer[2],pointer[3]]}
                    annex.append(contentsOf:[0,0,0,1]);annex.append(pointer,count:length)
                }
            }
        }
        var total=0;var pointer:UnsafeMutablePointer<Int8>?
        guard CMBlockBufferGetDataPointer(block,atOffset:0,lengthAtOffsetOut:nil,totalLengthOut:&total,dataPointerOut:&pointer)==noErr,let pointer=pointer else{return}
        let avcc=Data(bytes:pointer,count:total);var offset=0
        while offset+4<=avcc.count {
            let length=(Int(avcc[offset])<<24)|(Int(avcc[offset+1])<<16)|(Int(avcc[offset+2])<<8)|Int(avcc[offset+3]);offset+=4
            guard length>0,offset+length<=avcc.count else{return}
            annex.append(contentsOf:[0,0,0,1]);annex.append(avcc[offset..<offset+length]);offset+=length
        }
        guard offset==avcc.count else{return}
        let pts=CMTimeGetSeconds(CMSampleBufferGetPresentationTimeStamp(sample))
        if firstPTS==nil {firstPTS=pts}
        var packet=Data([key ? 1:0]);packet.u64(UInt64(max(0,(pts-(firstPTS ?? pts))*1_000_000)))
        packet.u16(UInt16(width));packet.u16(UInt16(height));packet.append(contentsOf:codecBytes);packet.append(annex)
        broadcast(packet,key:key)
    }
}

let app=NSApplication.shared
let delegate=ScreenSender()
app.delegate=delegate;app.setActivationPolicy(.accessory);app.run()
