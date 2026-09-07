#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# 视频 → 帧序列(.rgb 裸图 + meta.txt)，供板上 C++ video 模式闭环测试
# 用法(板上):
#   python3 video_to_frames.py <video.mp4> <outdir> [max_fps] [resize_width]
#   outdir 生成: f_0000.rgb... + meta.txt(w h fps frames)
#   例: python3 video_to_frames.py test.mp4 frames 10 640
import sys, os, cv2

def main():
    if len(sys.argv) < 3:
        print('用法: video_to_frames.py <mp4> <outdir> [max_fps] [resize_width]')
        sys.exit(1)
    src, outdir = sys.argv[1], sys.argv[2]
    max_fps = int(sys.argv[3]) if len(sys.argv) > 3 else 0
    resize_w = int(sys.argv[4]) if len(sys.argv) > 4 else 0

    cap = cv2.VideoCapture(src)
    if not cap.isOpened():
        print('[ERR] 打不开视频'); sys.exit(1)
    vfps = cap.get(cv2.CAP_PROP_FPS) or 30
    step = 1
    fps_out = int(round(vfps))
    if max_fps and vfps > max_fps:
        step = max(1, int(round(vfps / max_fps)))
        fps_out = max_fps
    print('视频 fps=%.1f, 抽帧: 每 %d 帧取 1 → 输出 %d fps' % (vfps, step, fps_out))

    os.makedirs(outdir, exist_ok=True)
    n = i = 0
    w = h = 0
    while True:
        ret, frame = cap.read()
        if not ret: break
        if i % step == 0:
            if resize_w:
                hh, ww = frame.shape[:2]
                rh = int(hh * resize_w / ww)
                frame = cv2.resize(frame, (resize_w, rh), interpolation=cv2.INTER_AREA)
            rgb = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
            h, w = rgb.shape[:2]
            rgb.tofile('%s/f_%04d.rgb' % (outdir, n))
            n += 1
        i += 1
    cap.release()
    with open('%s/meta.txt' % outdir, 'w') as f:
        f.write('%d\n%d\n%d\n%d\n' % (w, h, fps_out, n))
    print('[OK] 输出 %d 帧 %dx%d @%dfps → %s/' % (n, w, h, fps_out, outdir))

if __name__ == '__main__':
    main()
