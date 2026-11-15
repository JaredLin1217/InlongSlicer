const fs = require('fs');
const zlib = require('zlib');

const input = process.argv[2];
const output = process.argv[3];
const zmin = process.argv[4] === undefined ? -Infinity : Number(process.argv[4]);
const zmax = process.argv[5] === undefined ? Infinity : Number(process.argv[5]);
if (!input || !output) throw new Error('usage: node render_gcode_45deg.js input.gcode output.png [zmin] [zmax]');

const text = fs.readFileSync(input, 'utf8');
const lines = text.split(/\r?\n/);
const reXY = /X(-?(?:\d+(?:\.\d+)?|\.\d+))\s+Y(-?(?:\d+(?:\.\d+)?|\.\d+))/;
const reE = /\sE(-?(?:\d+(?:\.\d+)?|\.\d+))/;
const segments = [];
let x = 0, y = 0, z = 0, role = '';
for (const line of lines) {
  const s = line.trim();
  if (s.startsWith(';Z:')) z = Number(s.slice(3));
  if (s.startsWith(';TYPE:')) role = s.slice(6).trim();
  const xy = reXY.exec(s);
  const e = reE.exec(s);
  if (!xy) continue;
  const nx = Number(xy[1]), ny = Number(xy[2]);
  // Update the current position for travel moves too. Only positive-E moves
  // are extrusion segments; otherwise the travel itself would be drawn.
  if (e && Number(e[1]) > 0 && z >= zmin && z <= zmax && (nx !== x || ny !== y))
    segments.push({a:[x,y,z], b:[nx,ny,z], role});
  x = nx; y = ny;
}

// 45-degree isometric projection: azimuth 45°, elevation 45°.
const az = Math.PI / 4, el = Math.PI / 4;
function project(p) {
  const h = p[0] * Math.cos(az) + p[1] * Math.sin(az);
  const d = -p[0] * Math.sin(az) + p[1] * Math.cos(az);
  return [h, p[2] * Math.cos(el) - d * Math.sin(el)];
}
const projected = segments.map(s => ({...s, a:project(s.a), b:project(s.b)}));
const all = projected.flatMap(s => [s.a,s.b]);
if (!all.length) throw new Error('No extrusion segments found in selected Z range');
let minX=Math.min(...all.map(p=>p[0])), maxX=Math.max(...all.map(p=>p[0]));
let minY=Math.min(...all.map(p=>p[1])), maxY=Math.max(...all.map(p=>p[1]));
const W=1800,H=1100,pad=70;
const scale=Math.min((W-2*pad)/(maxX-minX||1),(H-2*pad)/(maxY-minY||1));
const pixels=Buffer.alloc(W*H*4);
// Opaque neutral background keeps the generated evidence readable without
// relying on a desktop screenshot or a viewer-specific theme.
for(let i=0;i<pixels.length;i+=4){pixels[i]=48;pixels[i+1]=48;pixels[i+2]=48;pixels[i+3]=255;}
function setPixel(px,py,r,g,b,a=255){ if(px<0||py<0||px>=W||py>=H)return; const i=(py*W+px)*4; pixels[i]=r;pixels[i+1]=g;pixels[i+2]=b;pixels[i+3]=a; }
function line(x0,y0,x1,y1,color,width){
  x0=Math.round(x0);y0=Math.round(y0);x1=Math.round(x1);y1=Math.round(y1);
  const dx=Math.abs(x1-x0),sx=x0<x1?1:-1,dy=-Math.abs(y1-y0),sy=y0<y1?1:-1;let err=dx+dy;
  const radius=Math.max(0,Math.floor(width/2));
  while(true){for(let ox=-radius;ox<=radius;ox++)for(let oy=-radius;oy<=radius;oy++)if(ox*ox+oy*oy<=radius*radius)setPixel(x0+ox,y0+oy,...color);if(x0===x1&&y0===y1)break;const e2=2*err;if(e2>=dy){err+=dy;x0+=sx}if(e2<=dx){err+=dx;y0+=sy}}
}
function xy(p){return [pad+(p[0]-minX)*scale,H-pad-(p[1]-minY)*scale];}
// Draw support first, then model, then interface on top.
const order={Support:0,'Support interface':2};
projected.sort((a,b)=>(order[a.role]??1)-(order[b.role]??1));
for(const s of projected){
  const role=s.role;
  const color=role==='Support interface'?[245,245,245,255]:role==='Support'?[25,205,55,255]:[225,105,25,255];
  const width=role==='Support interface'?7:role==='Support'?5:4;
  line(...xy(s.a),...xy(s.b),color,width);
}
// Simple color key in the top-left.
for(let i=0;i<3;i++){const c=[[225,105,25,255],[25,205,55,255],[245,245,245,255]][i];for(let yy=30+i*28;yy<46+i*28;yy++)for(let xx=30;xx<62;xx++)setPixel(xx,yy,...c);}

function crc32(buf){let c=0xffffffff;for(const v of buf){c^=v;for(let k=0;k<8;k++)c=(c>>>1)^((c&1)?0xedb88320:0)}return (c^0xffffffff)>>>0;}
function chunk(type,data){const t=Buffer.from(type);const out=Buffer.alloc(12+data.length);out.writeUInt32BE(data.length,0);t.copy(out,4);data.copy(out,8);out.writeUInt32BE(crc32(Buffer.concat([t,data])),8+data.length);return out;}
const raw=Buffer.alloc(H*(1+W*4));for(let yy=0;yy<H;yy++){raw[yy*(1+W*4)]=0;pixels.copy(raw,yy*(1+W*4)+1,yy*W*4,(yy+1)*W*4)}
const ihdr=Buffer.alloc(13);ihdr.writeUInt32BE(W,0);ihdr.writeUInt32BE(H,4);ihdr[8]=8;ihdr[9]=6;
const png=Buffer.concat([Buffer.from([137,80,78,71,13,10,26,10]),chunk('IHDR',ihdr),chunk('IDAT',zlib.deflateSync(raw,{level:9})),chunk('IEND',Buffer.alloc(0))]);
fs.writeFileSync(output,png);
console.log(JSON.stringify({output,segments:segments.length,zmin,zmax,bbox:[minX,minY,maxX,maxY],size:[W,H]}));
