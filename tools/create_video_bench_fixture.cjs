#!/usr/bin/env node
// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

'use strict';
// Original synthetic content. Output uses the native-video tests' framed format:
// 32-byte fixture key followed by one hvcC configuration and encrypted AUs.
const {spawnSync}=require('node:child_process');
const {createCipheriv}=require('node:crypto');
const {writeFileSync}=require('node:fs');
const [output,panel]=process.argv.slice(2);
if(!output||!['main','cluster'].includes(panel)||process.argv.length!==4)
  throw Error('Usage: node tools/create_video_bench_fixture.cjs NEW_PRIVATE_FILE main|cluster');
const count=60,source=`testsrc2=s=1920x720:r=60,hue=h=n*${panel==='main'?3:11}`;
const encoded=spawnSync(process.env.HEADUNIT_THEME_FFMPEG||'ffmpeg',[
  '-v','error','-f','lavfi','-i',source,'-frames:v',String(count),
  '-vf','scale=in_color_matrix=bt601:out_color_matrix=bt709',
  '-colorspace','bt709','-color_primaries','bt709','-color_trc','bt709',
  '-c:v','libx265','-preset','ultrafast',
  '-x265-params','pools=1:frame-threads=1:keyint=1:log-level=error',
  '-tag:v','hvc1','-movflags','frag_keyframe+empty_moov','-f','mp4','pipe:1',
],{timeout:120000,maxBuffer:16*1024*1024});
if(encoded.error)throw encoded.error;
if(encoded.status!==0)throw Error(encoded.stderr.toString());
const mp4=encoded.stdout,at=mp4.indexOf(Buffer.from('hvcC'));
if(at<4)throw Error('Encoder did not supply hvcC');
const size=mp4.readUInt32BE(at-4);
if(size<8||at-4+size>mp4.length)throw Error('Malformed encoder configuration');
const config=mp4.subarray(at+4,at-4+size),frames=[];
for(let i=0;i+8<=mp4.length;){
  const n=mp4.readUInt32BE(i);
  if(n<8||i+n>mp4.length)throw Error('Malformed encoder output');
  if(mp4.toString('ascii',i+4,i+8)==='mdat')frames.push(mp4.subarray(i+8,i+n));
  i+=n;
}
if(frames.length!==count)throw Error('Unexpected encoder frame count');
const key=Buffer.alloc(32,0x31);
function packet(opcode,body,index=0){
  const header=Buffer.alloc(128);header.writeUInt32LE(body.length+(opcode===0?16:0));
  header[4]=opcode;header[6]=2;
  if(opcode!==0)return Buffer.concat([header,body]);
  header.writeBigUInt64LE((900n<<32n)+BigInt(index)*4294967296n/60n,8);
  const iv=Buffer.alloc(12);iv.writeBigUInt64LE(BigInt(index),4);
  const cipher=createCipheriv('chacha20-poly1305',key,iv,{authTagLength:16});cipher.setAAD(header);
  return Buffer.concat([header,cipher.update(body),cipher.final(),cipher.getAuthTag()]);
}
const bytes=Buffer.concat([key,packet(1,config),...frames.map((f,i)=>packet(0,f,i))]);
if(bytes.length>16*1024*1024)throw Error('Fixture exceeds the replay bound');
writeFileSync(output,bytes,{flag:'wx',mode:0o600});
console.log(JSON.stringify({syntheticVideo:true,phoneCapture:false,width:1920,height:720,frames:count,bytes:bytes.length}));
