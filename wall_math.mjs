export const add=(a,b)=>a.map((v,i)=>v+b[i]);
export const sub=(a,b)=>a.map((v,i)=>v-b[i]);
export const scale=(a,s)=>a.map(v=>v*s);
export const dot=(a,b)=>a.reduce((s,v,i)=>s+v*b[i],0);
export const cross=(a,b)=>[a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]];
export const norm=a=>{const n=Math.hypot(...a);if(n<1e-7)throw Error('Points are too close');return scale(a,1/n);};
export const point=p=>[p.x,p.y,p.z];
export function multiply(a,b){const out=new Float32Array(16);for(let c=0;c<4;c++)for(let r=0;r<4;r++)for(let k=0;k<4;k++)out[c*4+r]+=a[k*4+r]*b[c*4+k];return out;}
export function transform(m,p){return [0,1,2].map(r=>m[r]*p[0]+m[4+r]*p[1]+m[8+r]*p[2]+m[12+r]);}
export function model(w,width,height){return new Float32Array([...scale(w.right,width/2),0,...scale(w.up,height/2),0,...w.normal,0,...w.center,1]);}
export function wallFromPoints(a,b,eye){
  const delta=sub(b,a);delta[1]=0;const distance=Math.hypot(...delta);
  if(distance<0.3)throw Error('Mark the other edge at least 30 cm away.');
  if(distance>5)throw Error('Choose two points closer together.');
  const center=scale(add(a,b),0.5);let normal=norm(cross(norm(delta),[0,1,0]));
  if(dot(normal,sub(eye,center))<0)normal=scale(normal,-1);
  const right=norm(cross([0,1,0],normal));
  return {center:add(center,scale(normal,0.015)),normal,right,up:[0,1,0],width:Math.min(distance,3.2),manual:true};
}
export function wallFromPlane(plane,pose,eye){
  const m=pose.transform.matrix;let normal=norm([m[4],0,m[6]]);
  const origin=[m[12],m[13],m[14]];
  if(dot(normal,sub(eye,origin))<0)normal=scale(normal,-1);
  const right=norm(cross([0,1,0],normal)),up=[0,1,0];
  const vertices=plane.polygon.map(p=>transform(m,point(p)));
  const xs=vertices.map(p=>dot(sub(p,origin),right)),ys=vertices.map(p=>dot(sub(p,origin),up));
  const bounds={left:Math.min(...xs),right:Math.max(...xs),bottom:Math.min(...ys),top:Math.max(...ys)};
  if(bounds.right-bounds.left<0.4||bounds.top-bounds.bottom<0.4)return null;
  const center=add(origin,add(scale(right,(bounds.left+bounds.right)/2),scale(up,(bounds.bottom+bounds.top)/2)));
  return {center:add(center,scale(normal,0.015)),normal,right,up,width:bounds.right-bounds.left,height:bounds.top-bounds.bottom,plane,bounds,origin};
}
export function intersect(ray,w,width=w.width,height=w.height){
  const divisor=dot(ray.direction,w.normal);if(Math.abs(divisor)<1e-5)return null;
  const distance=dot(sub(w.center,ray.origin),w.normal)/divisor;if(distance<=0||distance>12)return null;
  const position=add(ray.origin,scale(ray.direction,distance)),relative=sub(position,w.center);
  if(Math.abs(dot(relative,w.right))>width/2||Math.abs(dot(relative,w.up))>height/2)return null;
  return {distance,position,wall:w};
}
export function quaternion(w){
  const m00=w.right[0],m01=w.up[0],m02=w.normal[0],m10=w.right[1],m11=w.up[1],m12=w.normal[1],m20=w.right[2],m21=w.up[2],m22=w.normal[2];
  let x,y,z,q;const trace=m00+m11+m22;
  if(trace>0){const s=Math.sqrt(trace+1)*2;q=s/4;x=(m21-m12)/s;y=(m02-m20)/s;z=(m10-m01)/s;}
  else if(m00>m11&&m00>m22){const s=Math.sqrt(1+m00-m11-m22)*2;q=(m21-m12)/s;x=s/4;y=(m01+m10)/s;z=(m02+m20)/s;}
  else if(m11>m22){const s=Math.sqrt(1+m11-m00-m22)*2;q=(m02-m20)/s;x=(m01+m10)/s;y=s/4;z=(m12+m21)/s;}
  else{const s=Math.sqrt(1+m22-m00-m11)*2;q=(m10-m01)/s;x=(m02+m20)/s;y=(m12+m21)/s;z=s/4;}
  return {x,y,z,w:q};
}
