// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

'use strict';
const {test}=require('node:test');
const assert=require('node:assert/strict');
const {spawnSync}=require('node:child_process');
const {createCipheriv}=require('node:crypto');
test('native input path drives real decoder and GPU display receipts',()=>{
  const width=1280,height=720;
  const ffmpeg=process.env.HEADUNIT_THEME_FFMPEG,binary=process.env.HEADUNIT_THEME_NATIVE_INPUT_PROBE;assert(ffmpeg&&binary);
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
  const result=spawnSync(binary,[],{input:Buffer.concat([key,packet(1,config),...frames.map((f,i)=>packet(0,f,i))]),timeout:30000,maxBuffer:32768});
  assert.ifError(result.error);assert.equal(result.status,0,result.stderr.toString()+'\n'+result.stdout.toString());
  const text=result.stdout.toString().trim();
  const summary=JSON.parse(process.env.HEADUNIT_THEME_RECEIVER_VIDEO?text.split("\n").at(-1):text);
  assert.equal(summary.nativePresented,count);
  const checks=process.env.HEADUNIT_THEME_RECEIVER_VIDEO?['receiverRouting','twoSources','actualWaylandFeedback','syntheticClock','mainAfterClusterEof','freshClusterReopen','clusterAfterMainTeardown','freshMainReopen','retiredDuringComposition','controlRecordPreserved','disconnectWithdrawn']:['actualWaylandFeedback','syntheticClock','queueBounds','hiddenLayerHeld','clockRecovery','disconnectWithdrawn'];
  for(const key of checks)assert.equal(summary[key],true);
  if(process.env.HEADUNIT_THEME_TEST_TWO_PENDING && !process.env.HEADUNIT_THEME_RECEIVER_VIDEO)
    assert.equal(summary.freshBurst,true);
  console.log(JSON.stringify(summary));
});
