import {add,sub,scale,dot,norm,point,multiply,model,wallFromPoints,wallFromPlane,intersect,quaternion} from './wall_math.mjs';
const canvas=document.querySelector('#preview'),message=document.querySelector('#message'),enter=document.querySelector('#enter');
const gl=canvas.getContext('webgl',{alpha:true,xrCompatible:true,antialias:true});
const report={version:1,frames:0,placed:false,planes:0};
let session=null,space=null,lastPose=null,selected=null,anchor=null,manual=[],walls=[],hit=null,moveControl=null,lastFrame=null,videoTextureReady=false,videoWidth=1920,videoHeight=1248;
let decoder=null,decoderKey='',needsKey=true,streamMessage='Connecting to your Mac…',lastReport=0,dwellWall=null,dwellStart=0;
const inputs=new Map();
function say(text){message.textContent=text;streamMessage=text;}
function saveReport(){fetch('/report',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({...report,status:streamMessage,timestamp:Date.now()})}).catch(()=>{});}
function shader(type,source){const s=gl.createShader(type);gl.shaderSource(s,source);gl.compileShader(s);if(!gl.getShaderParameter(s,gl.COMPILE_STATUS))throw Error(gl.getShaderInfoLog(s));return s;}
const program=gl.createProgram();
gl.attachShader(program,shader(gl.VERTEX_SHADER,'attribute vec2 aPosition; uniform mat4 uMatrix; varying vec2 vUV; void main(){vUV=vec2((aPosition.x+1.0)*0.5,(1.0-aPosition.y)*0.5);gl_Position=uMatrix*vec4(aPosition,0.0,1.0);}'));
gl.attachShader(program,shader(gl.FRAGMENT_SHADER,'precision mediump float; varying vec2 vUV; uniform sampler2D uTexture; uniform float uOutline; void main(){if(uOutline>0.5&&vUV.x>0.008&&vUV.x<0.992&&vUV.y>0.008&&vUV.y<0.992)discard;gl_FragColor=texture2D(uTexture,vUV);}'));
gl.linkProgram(program);if(!gl.getProgramParameter(program,gl.LINK_STATUS))throw Error(gl.getProgramInfoLog(program));
const location={position:gl.getAttribLocation(program,'aPosition'),matrix:gl.getUniformLocation(program,'uMatrix'),outline:gl.getUniformLocation(program,'uOutline'),texture:gl.getUniformLocation(program,'uTexture')};
const vertices=gl.createBuffer();gl.bindBuffer(gl.ARRAY_BUFFER,vertices);gl.bufferData(gl.ARRAY_BUFFER,new Float32Array([-1,-1,1,-1,-1,1,-1,1,1,-1,1,1]),gl.STATIC_DRAW);
function texture(){const t=gl.createTexture();gl.bindTexture(gl.TEXTURE_2D,t);gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MIN_FILTER,gl.LINEAR);gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MAG_FILTER,gl.LINEAR);gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_WRAP_S,gl.CLAMP_TO_EDGE);gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_WRAP_T,gl.CLAMP_TO_EDGE);return t;}
const videoTexture=texture();
function card(lines,width=1024,height=280){const c=document.createElement('canvas');c.width=width;c.height=height;const ctx=c.getContext('2d');ctx.fillStyle='#163a33';ctx.fillRect(0,0,width,height);ctx.fillStyle='#e5f3df';ctx.textAlign='center';ctx.font='38px system-ui';lines.forEach((l,i)=>ctx.fillText(l,width/2,65+i*58,width-50));const t=texture();gl.texImage2D(gl.TEXTURE_2D,0,gl.RGBA,gl.RGBA,gl.UNSIGNED_BYTE,c);return t;}
let hudKey='',hudTexture=card(['Wall Screen','Connecting to your Mac…']);
const moveTexture=card(['Move screen'],768,150),edgeTexture=card(['•'],128,128),borderTexture=card([],16,16);
function draw(texture,matrix,outline=false){gl.useProgram(program);gl.bindBuffer(gl.ARRAY_BUFFER,vertices);gl.enableVertexAttribArray(location.position);gl.vertexAttribPointer(location.position,2,gl.FLOAT,false,0,0);gl.uniformMatrix4fv(location.matrix,false,matrix);gl.uniform1f(location.outline,outline?1:0);gl.activeTexture(gl.TEXTURE0);gl.bindTexture(gl.TEXTURE_2D,texture);gl.uniform1i(location.texture,0);gl.drawArrays(gl.TRIANGLES,0,6);}
function uploadVideo(){if(!lastFrame)return;try{gl.bindTexture(gl.TEXTURE_2D,videoTexture);gl.texImage2D(gl.TEXTURE_2D,0,gl.RGBA,gl.RGBA,gl.UNSIGNED_BYTE,lastFrame);const error=gl.getError();if(error!==gl.NO_ERROR)throw Error('Video texture upload: '+error);videoTextureReady=true;report.uploadedFrames=(report.uploadedFrames||0)+1;}catch(e){report.uploadError=String(e);}finally{lastFrame.close();lastFrame=null;}}
function handlePacket(data){
  const bytes=new Uint8Array(data);if(!bytes.length)return;
  if(bytes[0]===128){say(new TextDecoder().decode(bytes.subarray(1)));return;}
  if(bytes.length<20)return;
  const view=new DataView(data),key=!!(bytes[0]&1),width=view.getUint16(9),height=view.getUint16(11);
  const codec='avc1.'+Array.from(bytes.subarray(13,16),x=>x.toString(16).padStart(2,'0')).join('');
  if(!width||!height||width>4096||height>4096)return;
  const configKey=codec+':'+width+'x'+height;
  if(configKey!==decoderKey||!decoder||decoder.state==='closed'){
    if(!key)return;if(decoder&&decoder.state!=='closed')decoder.close();
    decoder=new VideoDecoder({output:frame=>{if(lastFrame)lastFrame.close();lastFrame=frame;report.frames++;say('Mac connected');},error:e=>{report.decoderError=String(e);needsKey=true;say('Reconnecting video…');}});
    decoder.configure({codec,codedWidth:width,codedHeight:height,optimizeForLatency:true,hardwareAcceleration:'prefer-hardware'});decoderKey=configKey;needsKey=true;videoWidth=width;videoHeight=height;
    report.video={width,height,codec};
  }
  if(decoder.decodeQueueSize>6){decoder.reset();decoder.configure({codec,codedWidth:width,codedHeight:height,optimizeForLatency:true,hardwareAcceleration:'prefer-hardware'});needsKey=true;}
  if(needsKey&&!key)return;if(key)needsKey=false;
  try{decoder.decode(new EncodedVideoChunk({type:key?'key':'delta',timestamp:Number(view.getBigUint64(1)),data:bytes.subarray(16)}));}catch(e){report.decoderError=String(e);needsKey=true;}
}
function connect(){const socket=new WebSocket('ws://'+locationHost()+'/stream');socket.binaryType='arraybuffer';socket.onmessage=e=>handlePacket(e.data);socket.onclose=()=>{needsKey=true;say('Connecting to your Mac…');setTimeout(connect,2000);};socket.onerror=()=>socket.close();}
function locationHost(){return window.location.host;}
function cameraBasis(pose){const m=pose.transform.matrix;return {center:point(pose.transform.position),right:[m[0],m[1],m[2]],up:[m[4],m[5],m[6]],normal:[m[8],m[9],m[10]]};}
function gaze(pose){const b=cameraBasis(pose);return {origin:b.center,direction:scale(b.normal,-1)};}
function restartPlacement(){if(anchor){anchor.delete();anchor=null;}selected=null;manual=[];report.placed=false;localStorage.removeItem('wall-screen-anchor');say('Choose a wall');}
document.querySelector('#move').onclick=restartPlacement;
async function place(wall,frame){
  selected=wall;manual=[];report.placed=true;report.placement=wall.manual?'marked-wall':'detected-wall';report.width=wall.width;
  if(frame.createAnchor){try{
    anchor=await frame.createAnchor(new XRRigidTransform({x:wall.center[0],y:wall.center[1],z:wall.center[2]},quaternion(wall)),space);
    if(anchor.requestPersistentHandle){const handle=await anchor.requestPersistentHandle();localStorage.setItem('wall-screen-anchor',JSON.stringify({handle,width:wall.width}));report.persistentAnchor=true;}
  }catch(e){report.anchorError=String(e);}}
  saveReport();
}
function useSelection(source,frame){
  const data=inputs.get(source);const ray=data?.ray||(lastPose?gaze(lastPose):null);if(!ray||!lastPose)return;
  if(selected){if(moveControl&&intersect(ray,moveControl,0.5,0.13))restartPlacement();return;}
  const candidate=walls.map(w=>intersect(ray,w)).filter(Boolean).sort((a,b)=>a.distance-b.distance)[0];
  if(candidate){const w=candidate.wall;const width=Math.min(2.6,w.width*0.85,w.height*0.8*videoWidth/videoHeight);place({...w,width,center:add(w.center,scale(w.up,Math.max(-w.height/4,Math.min(w.height/4,ray.origin[1]-w.center[1]))))},frame);return;}
  if(!data?.touch){say('Use a tracked hand or controller to mark the wall.');return;}
  manual.push(data.touch);
  if(manual.length===2){try{place(wallFromPoints(manual[0],manual[1],gaze(lastPose).origin),frame);}catch(e){manual.pop();say(e.message);}}
}
function instructions(){
  if(walls.length)return ['Choose your wall','Point and select, or look steadily for 3 seconds.'];
  return manual.length?['Mark the right edge','Touch the wall and pinch or press trigger.','Use the same middle height as the first point.']:['Mark the left edge of your screen','Touch the wall and pinch or press trigger.','Then mark the right edge at the same height.'];
}
function frame(time,frame){
  session.requestAnimationFrame((t,f)=>windowFrame(t,f));
  const pose=frame.getViewerPose(space);if(!pose)return;lastPose=pose;uploadVideo();
  inputs.clear();
  for(const source of session.inputSources){
    const rayPose=frame.getPose(source.targetRaySpace,space);let touch=null;
    if(source.hand){const joint=source.hand.get('index-finger-tip');const p=joint&&frame.getJointPose(joint,space);if(p)touch=point(p.transform.position);}
    if(!touch&&source.gripSpace){const p=frame.getPose(source.gripSpace,space);if(p)touch=point(p.transform.position);}
    if(rayPose){const m=rayPose.transform.matrix;inputs.set(source,{touch,ray:{origin:point(rayPose.transform.position),direction:[-m[8],-m[9],-m[10]]}});}
  }
  report.inputSources=inputs.size;report.trackedHands=Array.from(session.inputSources).filter(s=>s.hand).length;report.markedPoints=manual.length;
  walls=[];
  for(const plane of frame.detectedPlanes||[]){if(plane.orientation!=='vertical')continue;const label=(plane.semanticLabel||'').toLowerCase();if(label&&!label.includes('wall'))continue;const p=frame.getPose(plane.planeSpace,space);if(!p)continue;try{const w=wallFromPlane(plane,p,gaze(pose).origin);if(w)walls.push(w);}catch{}}
  report.planes=walls.length;
  if(!selected&&walls.length){hit=walls.map(w=>intersect(gaze(pose),w)).filter(Boolean).sort((a,b)=>a.distance-b.distance)[0];if(hit){if(dwellWall!==hit.wall.plane){dwellWall=hit.wall.plane;dwellStart=time;}if(time-dwellStart>3000)useSelection(null,frame);}else{dwellWall=null;}}
  if(anchor&&selected){const p=frame.getPose(anchor.anchorSpace,space);if(p){const b=cameraBasis(p);selected={...selected,...b};}}
  const layer=session.renderState.baseLayer;gl.bindFramebuffer(gl.FRAMEBUFFER,layer.framebuffer);gl.clearColor(0,0,0,0);gl.clear(gl.COLOR_BUFFER_BIT|gl.DEPTH_BUFFER_BIT);gl.enable(gl.BLEND);gl.blendFunc(gl.SRC_ALPHA,gl.ONE_MINUS_SRC_ALPHA);
  const lines=instructions(),key=lines.join('|')+'|'+streamMessage;
  if(!selected&&key!==hudKey){if(hudTexture)gl.deleteTexture(hudTexture);hudTexture=card([...lines,streamMessage],1280,330);hudKey=key;}
  const head=cameraBasis(pose),hud={...head,center:add(head.center,scale(head.normal,-1.15))};
  for(const view of pose.views){
    const v=layer.getViewport(view);gl.viewport(v.x,v.y,v.width,v.height);const pv=multiply(view.projectionMatrix,view.transform.inverse.matrix);
    if(selected){
      const height=selected.width*videoHeight/videoWidth;
      if(dot(selected.normal,sub(head.center,selected.center))>0){draw(videoTextureReady?videoTexture:hudTexture,multiply(pv,model(selected,selected.width,height)));moveControl={...selected,center:add(selected.center,add(scale(selected.up,height/2+0.12),scale(selected.right,selected.width/2-0.25)))};draw(moveTexture,multiply(pv,model(moveControl,0.5,0.13)));}
    }else{
      for(const wall of walls)draw(borderTexture,multiply(pv,model(wall,wall.width,wall.height)),true);
      for(const p of manual){const marker={...head,center:p};draw(edgeTexture,multiply(pv,model(marker,0.06,0.06)));}
      draw(hudTexture,multiply(pv,model(hud,0.95,0.245)));
    }
  }
  if(time-lastReport>1500){lastReport=time;saveReport();}
}
const windowFrame=(t,f)=>frame(t,f);
enter.onclick=async()=>{
  try{
    session=await navigator.xr.requestSession('immersive-ar',{requiredFeatures:['local-floor'],optionalFeatures:['plane-detection','anchors','hand-tracking']});
    await gl.makeXRCompatible();session.updateRenderState({baseLayer:new XRWebGLLayer(session,gl)});space=await session.requestReferenceSpace('local-floor');
    session.addEventListener('select',e=>useSelection(e.inputSource,e.frame));session.addEventListener('end',()=>{session=null;requestAnimationFrame(preview);});
    const saved=localStorage.getItem('wall-screen-anchor');if(saved&&session.restorePersistentAnchor){try{const data=JSON.parse(saved);anchor=await session.restorePersistentAnchor(data.handle);selected={center:[0,0,-2],right:[1,0,0],up:[0,1,0],normal:[0,0,1],width:data.width};report.placed=true;}catch{localStorage.removeItem('wall-screen-anchor');}}
    session.requestAnimationFrame(windowFrame);report.immersive=true;saveReport();
  }catch(e){say('Room view could not start: '+e.message);report.xrError=String(e);saveReport();}
};
function preview(){if(session)return;uploadVideo();const width=canvas.clientWidth||900,height=Math.round(width*videoHeight/videoWidth);if(canvas.width!==width||canvas.height!==height){canvas.width=width;canvas.height=height;}gl.bindFramebuffer(gl.FRAMEBUFFER,null);gl.viewport(0,0,canvas.width,canvas.height);gl.clearColor(0.03,0.08,0.07,1);gl.clear(gl.COLOR_BUFFER_BIT);const identity=new Float32Array([1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]);draw(videoTextureReady?videoTexture:hudTexture,identity);requestAnimationFrame(preview);}
if(!gl||!window.VideoDecoder||!navigator.xr){say('This browser does not support the viewer.');enter.disabled=true;}else{connect();requestAnimationFrame(preview);setInterval(saveReport,2000);}
