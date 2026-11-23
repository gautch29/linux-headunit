// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

'use strict';
// FFmpeg independently encodes the fixture. Node seals the wire packets. The
// probe runs the C++ parser and real GStreamer decoder, checking pixels and PTS.
const {test}=require('node:test');
const assert=require('node:assert/strict');
const {spawnSync}=require('node:child_process');
const {createCipheriv}=require('node:crypto');
const binary=process.env.HEADUNIT_THEME_VIDEO_DECODER_PROBE,ffmpeg=process.env.HEADUNIT_THEME_FFMPEG;
assert(binary && ffmpeg,'decoder probe and ffmpeg are required');
const key=Buffer.alloc(32,0x31);
function message(opcode,payload,{ntp=0n,nonce=0n,flags=2}={}) {
  const header=Buffer.alloc(128);header.writeUInt32LE(payload.length+(opcode===0?16:0));header[4]=opcode;
  header.writeUInt16LE(flags,6);header.writeBigUInt64LE(ntp,8);
  if(opcode!==0)return Buffer.concat([header,payload]);
  const iv=Buffer.alloc(12);iv.writeBigUInt64LE(nonce,4);
  const cipher=createCipheriv('chacha20-poly1305',key,iv,{authTagLength:16});cipher.setAAD(header);
  return Buffer.concat([header,cipher.update(payload),cipher.final(),cipher.getAuthTag()]);
}
function fixture(count=96) {
  const encoded=spawnSync(ffmpeg,['-v','error','-f','lavfi','-i','color=c=red:s=64x48:r=30','-frames:v',String(count),
    '-c:v','libx264','-preset','ultrafast','-tune','zerolatency','-profile:v','baseline','-x264-params','aud=1:keyint=1',
    '-f','h264','pipe:1'],{timeout:10000,maxBuffer:1024*1024});
  assert.ifError(encoded.error);assert.equal(encoded.status,0,encoded.stderr.toString());
  const data=encoded.stdout,starts=[];
  for(let i=0;i+3<data.length;++i) {
    if(data[i]===0&&data[i+1]===0&&data[i+2]===1){starts.push([i,i+3]);i+=2;}
    else if(data[i]===0&&data[i+1]===0&&data[i+2]===0&&data[i+3]===1){starts.push([i,i+4]);i+=3;}
  }
  const nals=starts.map((s,i)=>data.subarray(s[1],starts[i+1]?.[0]??data.length));
  const sps=nals.find(n=>(n[0]&31)===7),pps=nals.find(n=>(n[0]&31)===8);assert(sps&&pps);
  const length16=b=>{const n=Buffer.alloc(2);n.writeUInt16BE(b.length);return Buffer.concat([n,b]);};
  const config=Buffer.concat([Buffer.from([1,...sps.subarray(1,4),0xff,0xe1]),length16(sps),Buffer.from([1]),length16(pps)]);
  const units=[];
  for(const nal of nals){if((nal[0]&31)===9)units.push([]);assert(units.length);const n=Buffer.alloc(4);n.writeUInt32BE(nal.length);units.at(-1).push(n,nal);}
  assert.equal(units.length,count);
  return {config,frames:units.map(u=>Buffer.concat(u))};
}
const source=fixture();
function run(config,frames,stamps,flags=2,decoder){
  const packets=[message(1,config,{flags}),...frames.map((frame,i)=>message(0,frame,{ntp:stamps[i],nonce:BigInt(i)}))];
  const child=spawnSync(binary,decoder?[decoder]:[],{input:Buffer.concat([key,...packets]),timeout:20000,maxBuffer:1024*1024});
  assert.ifError(child.error);
  return {...child,frames:child.stdout.toString().trim().split('\n').filter(Boolean).map(line=>JSON.parse(line))};
}
test('96 encrypted frames decode with bounded backpressure, exact source timestamps and correct pixels',()=>{
  // Fractional bits deliberately do not round-trip through nanoseconds/double.
  const stamps=source.frames.map((_,i)=>0xf123456789abcdefn+(BigInt(i)*4294967296n/30n));
  const result=run(source.config,source.frames,stamps);assert.equal(result.status,0,result.stderr.toString());
  assert.match(result.stderr.toString(),/decoder=avdec_h264/);assert.equal(result.frames.length,stamps.length);
  for(let i=0;i<stamps.length;++i){
    const frame=result.frames[i],ntp=stamps[i];assert.equal(frame.ntp,ntp.toString());
    assert.equal(frame.ns,((ntp>>32n)*1000000000n+((ntp&0xffffffffn)*1000000000n>>32n)).toString());
    assert.equal(frame.width,64);assert.equal(frame.height,48);assert(frame.rgb[0]>=245&&frame.rgb[1]<=5&&frame.rgb[2]<=5);
  }
});
test('repeated authenticated source timestamp keeps distinct decoder units and exact metadata',()=>{
  const stamp=0x123456789n;
  const result=run(source.config,source.frames.slice(0,2),[stamp,stamp]);
  assert.equal(result.status,0,result.stderr.toString());
  assert.equal(result.frames.length,2);
  for(const frame of result.frames){
    assert.equal(frame.ntp,stamp.toString());
    assert.equal(frame.ns,((stamp>>32n)*1000000000n+((stamp&0xffffffffn)*1000000000n>>32n)).toString());
  }
});
test('disabled sender timestamps cannot become fabricated decoder time',()=>{
  const result=run(source.config,source.frames.slice(0,1),[123n],0);
  assert.equal(result.status,1);assert.equal(result.frames.length,0);assert.match(result.stderr.toString(),/requires a timestamped/);
});
test('wrapped AVC configuration reaches the real decoder without box bytes or adjacent metadata',()=>{
  const atom=(name,body)=>{const h=Buffer.alloc(8);h.writeUInt32BE(body.length+8);h.write(name,4);return Buffer.concat([h,body]);};
  const config=atom('avc1',Buffer.concat([Buffer.alloc(78),atom('avcC',source.config),atom('pasp',Buffer.alloc(8)),Buffer.alloc(4)]));
  const result=run(config,source.frames.slice(0,3),[1n<<32n,2n<<32n,3n<<32n]);
  assert.equal(result.status,0,result.stderr.toString());assert.equal(result.frames.length,3);
});
function hevcFixture(width=64,height=48){
  const encoded=spawnSync(ffmpeg,['-v','error','-f','lavfi','-i',`color=c=blue:s=${width}x${height}:r=30`,'-frames:v','1',
    // lavfi's YUV colour is BT.601. Convert the samples AND signal BT.709 VUI;
    // an unspecified matrix lets SD/HD decoders make different valid guesses.
    '-vf','scale=in_color_matrix=bt601:out_color_matrix=bt709','-colorspace','bt709','-color_primaries','bt709','-color_trc','bt709',
    '-c:v','libx265','-preset','ultrafast','-x265-params','pools=1:frame-threads=1:keyint=1:log-level=error',
    '-tag:v','hvc1','-movflags','frag_keyframe+empty_moov','-f','mp4','pipe:1'],{timeout:10000,maxBuffer:1024*1024});
  assert.ifError(encoded.error);assert.equal(encoded.status,0,encoded.stderr.toString());
  const mp4=encoded.stdout,at=mp4.indexOf(Buffer.from('hvcC'));assert(at>=4);
  const length=mp4.readUInt32BE(at-4),config=mp4.subarray(at+4,at-4+length);let frame;
  for(let i=0;i+8<=mp4.length;){const n=mp4.readUInt32BE(i);assert(n>=8&&i+n<=mp4.length);
    if(mp4.toString('ascii',i+4,i+8)==='mdat'){assert(!frame);frame=mp4.subarray(i+8,i+n);}i+=n;}
  const atom=(name,body)=>{const h=Buffer.alloc(8);h.writeUInt32BE(body.length+8);h.write(name,4);return Buffer.concat([h,body]);};
  const description=atom('hvc1',Buffer.concat([Buffer.alloc(78),atom('hvcC',config),atom('colr',Buffer.alloc(10)),Buffer.alloc(4)]));
  assert(frame);return {description,frame};
}
test('independently encoded HEVC settings and access unit decode with preserved source timestamp',()=>{
  const {description,frame}=hevcFixture(),stamp=0x123456789abcdef0n,result=run(description,[frame],[stamp]);
  assert.equal(result.status,0,result.stderr.toString());assert.match(result.stderr.toString(),/decoder=avdec_h265/);
  assert.equal(result.frames.length,1);assert.equal(result.frames[0].ntp,stamp.toString());
  assert(result.frames[0].rgb[2]>=245 && result.frames[0].rgb[0]<=5 && result.frames[0].rgb[1]<=5);
});
test('opt-in hardware HEVC decode produces 720p pixels and preserves source timestamp',{
  skip:!process.env.HEADUNIT_THEME_HARDWARE_HEVC_DECODER,
},()=>{
  const decoder=process.env.HEADUNIT_THEME_HARDWARE_HEVC_DECODER;
  assert(['v4l2slh265dec','v4l2h265dec','vah265dec'].includes(decoder));
  const {description,frame}=hevcFixture(1280,720),stamp=0x123456789abcdef0n,result=run(description,[frame],[stamp],2,decoder);
  assert.equal(result.status,0,result.stderr.toString());assert(result.stderr.toString().includes('decoder='+decoder));
  assert.equal(result.frames.length,1);assert.equal(result.frames[0].ntp,stamp.toString());
  assert.equal(result.frames[0].width,1280);assert.equal(result.frames[0].height,720);
  assert(result.frames[0].rgb[2]>=245 && result.frames[0].rgb[0]<=5 && result.frames[0].rgb[1]<=5,JSON.stringify(result.frames[0]));
  const software=run(description,[frame],[stamp],2,'avdec_h265');assert.equal(software.status,0,software.stderr.toString());
  for(let channel=0;channel<3;++channel)assert(Math.abs(result.frames[0].rgb[channel]-software.frames[0].rgb[channel])<=2);

});
test('malformed codec configuration fails without publishing decoded output',()=>{
  const result=run(Buffer.alloc(4),[],[]);assert.equal(result.status,1);assert.equal(result.frames.length,0);
});
