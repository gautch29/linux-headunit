// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

'use strict';
const {test}=require('node:test');
const assert=require('node:assert/strict');
const {spawnSync}=require('node:child_process');
const {createCipheriv}=require('node:crypto');
for(const [width,height,stress,leases] of [[1280,720,true,3],[1280,720,false,4],[1278,718,false,3]])test(`Pi native GPU composition preserves changing frames at ${width}x${height} with ${stress?'66':'2'} overlays and ${leases} leases`,()=>{
  const ffmpeg=process.env.HEADUNIT_THEME_FFMPEG,binary=process.env.HEADUNIT_THEME_GPU_VIDEO_PROBE;assert(ffmpeg&&binary);
  const count=12,source=`color=c=blue:s=${width}x${height}:r=30,drawbox=x=640:y=0:w=640:h=352:c=red:t=fill,drawbox=x=0:y=352:w=640:h=368:c=yellow:t=fill,hue=h=n*17`;
  const encoded=spawnSync(ffmpeg,['-v','error','-f','lavfi','-i',source,'-frames:v',String(count),
    '-vf','scale=in_color_matrix=bt601:out_color_matrix=bt709','-colorspace','bt709','-color_primaries','bt709','-color_trc','bt709',
    '-c:v','libx265','-preset','ultrafast','-x265-params','pools=1:frame-threads=1:keyint=1:log-level=error',
    '-tag:v','hvc1','-movflags','frag_keyframe+empty_moov','-f','mp4','pipe:1'],{timeout:15000,maxBuffer:4*1024*1024});
  assert.ifError(encoded.error);assert.equal(encoded.status,0,encoded.stderr.toString());
  const mp4=encoded.stdout,at=mp4.indexOf(Buffer.from('hvcC'));assert(at>=4);
  const config=mp4.subarray(at+4,at-4+mp4.readUInt32BE(at-4)),frames=[];
  for(let i=0;i+8<=mp4.length;){const n=mp4.readUInt32BE(i);assert(n>=8&&i+n<=mp4.length);
    if(mp4.toString('ascii',i+4,i+8)==='mdat')frames.push(mp4.subarray(i+8,i+n));i+=n;}
  assert.equal(frames.length,count);
  const key=Buffer.alloc(32,0x31),stamps=frames.map((_,i)=>0x123456789abcdef0n+BigInt(i)*4294967296n/30n);
  function packet(opcode,body,index=0){
    const header=Buffer.alloc(128);header.writeUInt32LE(body.length+(opcode===0?16:0));header[4]=opcode;header[6]=2;
    if(opcode!==0)return Buffer.concat([header,body]);
    header.writeBigUInt64LE(stamps[index],8);const iv=Buffer.alloc(12);iv.writeBigUInt64LE(BigInt(index),4);
    const cipher=createCipheriv('chacha20-poly1305',key,iv,{authTagLength:16});cipher.setAAD(header);
    return Buffer.concat([header,cipher.update(body),cipher.final(),cipher.getAuthTag()]);
  }
  const args=['/dev/dri/renderD128'];if(process.env.HEADUNIT_THEME_GPU_PROBE_PNG)args.push(process.env.HEADUNIT_THEME_GPU_PROBE_PNG);
  const result=spawnSync(binary,args,{input:Buffer.concat([key,packet(1,config),...frames.map((f,i)=>packet(0,f,i))]),timeout:30000,maxBuffer:32768,
    env:{...process.env,HEADUNIT_THEME_GPU_PROBE_OVERLAY_STRESS:stress?'1':undefined,
      HEADUNIT_THEME_GPU_PROBE_FOUR_LEASES:leases===4?'1':undefined}});
  assert.ifError(result.error);assert.equal(result.status,0,result.stderr.toString()+'\n'+result.stdout.toString());
  const rows=result.stdout.toString().trim().split('\n').map(x=>JSON.parse(x));
  const decoded=rows.filter(x=>x.event==='frame');assert.equal(decoded.length,count);
  for(const [i,r] of decoded.entries()){assert.equal(r.ntp,stamps[i].toString());assert.equal(r.width,width);assert.equal(r.height,height);assert(r.planes>0);assert(r.maxChannelError<=5);}
  const wayland=Boolean(process.env.HEADUNIT_THEME_GPU_PROBE_WAYLAND);
  const summary=rows.at(-1);assert.equal(summary.frames,count);assert.equal(summary.leaseBoundTested,true);assert.equal(summary.leaseSurvivesDecoder,true);assert.match(summary.renderer,/V3D/);assert.equal(summary.presented,wayland);
  assert.equal(summary.nativeLeaseLimit,leases);
  assert.equal(summary.overlays,stress?66:2);assert(summary.meanDrawNs>0);
  assert.equal(summary.nativeUiInputTested,true);assert.equal(summary.presentations,wayland?count+2:0);
  assert.equal(summary.referenceOutputTested,true);
  assert.equal(summary.presentationClockMapped,wayland);assert.equal(summary.syntheticMediaClock,wayland);
  const receipts=rows.filter(x=>x.event==='presentation');assert.equal(receipts.length,wayland?count:0);
  for(const [i,r] of receipts.entries()){
    assert.equal(r.token,i+1);assert(r.nanoseconds>=0&&r.nanoseconds<1e9);
    assert([1,4].includes(r.clockId),'fixture compositor must use a monotonic clock');
    assert.equal(r.syntheticMediaClock,true);assert(r.mediaPTS>=900&&r.mediaPTS<925);
    assert(Number.isSafeInteger(r.uncertaintyNs)&&r.uncertaintyNs>=1000&&r.uncertaintyNs<=25000000);
    if(i)assert(r.mediaPTS>=receipts[i-1].mediaPTS);
    if(i)assert(BigInt(r.seconds)*1000000000n+BigInt(r.nanoseconds)>=BigInt(receipts[i-1].seconds)*1000000000n+BigInt(receipts[i-1].nanoseconds));
  }
  console.log(JSON.stringify({summary,first:decoded[0],last:decoded.at(-1),
    presentationClockIds:[...new Set(receipts.map(x=>x.clockId))],presentationFlags:[...new Set(receipts.map(x=>x.flags))]}));
});
