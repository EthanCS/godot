#!/usr/bin/env python3
"""Create a self-contained HTML review from unmodified Sponza GPU captures."""
import argparse
import base64
import json
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('captures', type=Path)
parser.add_argument('output', type=Path)
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
images = {}
for hour in ['0650', '0900', '1200', '1750', '2100']:
    images[hour] = {}
    for mode in ['on','off','indirect']:
        data = (args.captures / ('tod_'+hour+'_'+mode) / 'color.png').read_bytes()
        images[hour][mode] = 'data:image/png;base64,' + base64.b64encode(data).decode()
html = r"""<!doctype html><html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Sponza · TOD 漫反射 GI</title>
<style>
*{box-sizing:border-box}body{margin:0;background:#121719;color:#eef1ed;font:16px/1.6 system-ui,sans-serif}main{max-width:1280px;margin:auto;padding:32px 24px}h1{font-size:30px;margin:0 0 5px}p{color:#afb9b5;margin:5px 0 22px}video{width:100%;max-height:570px;background:#050708;border-radius:12px}nav{display:flex;gap:8px;flex-wrap:wrap;margin:26px 0 12px}button{font:inherit;border:1px solid #45524c;color:#dfe6df;background:#202925;padding:8px 18px;border-radius:24px;cursor:pointer}button.active{background:#dfbe82;color:#171b18;border-color:#dfbe82}.view{position:relative;aspect-ratio:16/9;overflow:hidden;background:black;border-radius:12px}.view img{position:absolute;width:100%;height:100%;object-fit:contain}.badge{position:absolute;top:12px;z-index:2;background:#08100ddd;padding:4px 12px;border-radius:6px}.left{left:12px}.right{right:12px}#wipe{position:absolute;top:0;bottom:0;width:2px;background:#fff9;left:50%}input{width:100%;accent-color:#dfbe82;margin:18px 0}.row{display:flex;justify-content:space-between;gap:18px;align-items:center;flex-wrap:wrap}.note{font-size:14px;color:#92a19a;margin-top:28px}a{color:#dfbe82}
</style><main><h1>Sponza · 日照与漫反射 GI</h1><p>太阳 + 天空。局部灯与自发光道具全部关闭。</p>
<video controls loop preload="metadata" src="tod.mp4"></video><p>20 秒展示完整 24 小时循环，相机固定。视频经过编码；下方对照使用原始 GPU 截图。</p>
<nav id="times"></nav><div class="row"><p id="caption"></p><button id="mode">只看间接光</button></div>
<div class="view"><img id="off"><img id="on"><div class="badge left" id="left">GI 开启</div><div class="badge right" id="right">GI 关闭</div><div id="wipe"></div></div>
<input aria-label="GI 开关对比位置" id="slider" type="range" min="0" max="100" value="50"><p>拖动滑条，比较同一时刻、同一机位的遮阴走廊、地面和布幔反弹光。</p>
<p class="note">两种查询后端共用导入代理、材质和降噪。此页展示 Windows / Vulkan / RTX 5070 Ti 的硬件查询结果；Mac 尚未验收。实时体验可运行仓库中的 kiln/sponza/run.ps1，拖动 TOD 滑条或按空格播放。</p></main>
<script>const images=__IMAGES__;const times=[['0650','06:30 · 清晨'],['0900','09:00 · 上午'],['1200','12:00 · 正午'],['1750','17:30 · 日落'],['2100','21:00 · 夜晚']];let current='1200',indirect=false;const $=id=>document.getElementById(id);function refresh(){ $('off').src=images[current].off;$('on').src=images[current][indirect?'indirect':'on'];$('on').style.clipPath=indirect?'none':`inset(0 ${100-$('slider').value}% 0 0)`;$('wipe').style.left=$('slider').value+'%';$('wipe').hidden=indirect;$('slider').hidden=indirect;$('right').hidden=indirect;$('left').textContent=indirect?'只显示间接光':'GI 开启';$('mode').textContent=indirect?'返回 GI 开关对比':'只看间接光';$('caption').textContent=times.find(t=>t[0]===current)[1]+' · 固定曝光 / 固定机位';document.querySelectorAll('nav button').forEach(b=>b.classList.toggle('active',b.dataset.hour===current));}for(const [hour,label] of times){let b=document.createElement('button');b.textContent=label;b.dataset.hour=hour;b.onclick=()=>{current=hour;refresh()};$('times').append(b)}$('slider').oninput=refresh;$('mode').onclick=()=>{indirect=!indirect;refresh()};refresh();</script></html>"""
(args.output/'index.html').write_text(html.replace('__IMAGES__', json.dumps(images)), encoding='utf-8')
print(args.output/'index.html')
