import test from 'node:test';
import assert from 'node:assert/strict';
import {wallFromPoints,intersect,dot,quaternion} from './wall_math.mjs';
test('marked wall stays upright, faces the viewer, and accepts its center ray',()=>{
 const w=wallFromPoints([-1,1.5,-2],[1,1.5,-2],[0,1.5,0]);
 assert.deepEqual(w.up,[0,1,0]);assert.equal(w.width,2);assert.ok(w.normal[2]>0.99);
 assert.ok(Math.abs(dot(w.normal,w.right))<1e-8);
 assert.ok(intersect({origin:[0,1.5,0],direction:[0,0,-1]},w,w.width,1.25));
 const q=quaternion(w);assert.ok(Math.abs(Math.hypot(q.x,q.y,q.z,q.w)-1)<1e-8);
});
test('reversing the marks does not mirror the screen',()=>{
 const w=wallFromPoints([1,1.5,-2],[-1,1.5,-2],[0,1.5,0]);assert.ok(w.right[0]>0.99);assert.ok(w.normal[2]>0.99);
});
test('off-screen and backwards rays do not select a wall',()=>{
 const w=wallFromPoints([-1,1.5,-2],[1,1.5,-2],[0,1.5,0]);
 assert.equal(intersect({origin:[4,1.5,0],direction:[0,0,-1]},w,2,1),null);
 assert.equal(intersect({origin:[0,1.5,0],direction:[0,0,1]},w,2,1),null);
});
test('ambiguous marks are rejected instead of creating an arbitrary wall',()=>{
 assert.throws(()=>wallFromPoints([0,1,-2],[0.01,1,-2],[0,1,0]));
});
