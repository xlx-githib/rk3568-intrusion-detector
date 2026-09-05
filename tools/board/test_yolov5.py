#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# yolov5 单图验证（D3）：在板上用 RKNNLite 跑 yolov5.rknn，只留 person/car，画框存图
# 用法(板端, 与模型/图片同目录):
#   python3 test_yolov5.py yolov5.rknn bus.jpg out.jpg
# 说明: 后处理逻辑与官方 yolo.cc 的 fp 解码一致(见 study/03)。
#   首次跑若解码异常, 请把脚本打印的输出 shape 贴回来, 据此调整布局。
import sys
import cv2
import numpy as np
from rknnlite.api import RKNNLite

# ---- yolov5 超参(与官方一致) ----
STRIDES   = [8, 16, 32]
ANCHORS   = [[10,13,16,30,33,23],
             [30,61,62,45,59,119],
             [116,90,156,198,373,326]]
COCO_NAMES = ['person','bicycle','car','motorbike','aeroplane','bus','train','truck',
              'boat','traffic light','fire hydrant','stop sign','parking meter','bench',
              'bird','cat','dog','horse','sheep','cow','elephant','bear','zebra','giraffe',
              'backpack','umbrella','handbag','tie','suitcase','frisbee','skis','snowboard',
              'sports ball','kite','baseball bat','baseball glove','skateboard','surfboard',
              'tennis racket','bottle','wine glass','cup','fork','knife','spoon','bowl','banana',
              'apple','sandwich','orange','broccoli','carrot','hot dog','pizza','donut','cake',
              'chair','sofa','pottedplant','bed','diningtable','toilet','tvmonitor','laptop','mouse',
              'remote','keyboard','cell phone','microwave','oven','toaster','sink','refrigerator',
              'book','clock','vase','scissors','teddy bear','hair drier','toothbrush']
WHITELIST = {0, 2}          # 只留 person(0) / car(2)
CONF_TH   = 0.25
NMS_TH    = 0.45
NUM_CLS   = 80
BOX_SIZE  = 5 + NUM_CLS     # 85


def letterbox(img, dst=640):
    """等比例缩放 + 灰边补成 dst×dst, 返回 (img640, scale, padx, pady)"""
    h, w = img.shape[:2]
    scale = min(dst / w, dst / h)
    nw, nh = int(w * scale), int(h * scale)
    resized = cv2.resize(img, (nw, nh), interpolation=cv2.INTER_AREA)
    canvas = np.full((dst, dst, 3), 114, dtype=np.uint8)   # 灰边 114(与官方一致)
    padx, pady = (dst - nw) // 2, (dst - nh) // 2
    canvas[pady:pady+nh, padx:padx+nw] = resized
    return canvas, scale, padx, pady


def decode_one(output, stride, anchors):
    """解码一路输出(fp), 逻辑同 yolo.cc 的 process_fp; 返回 boxes[x,y,w,h],scores,cls"""
    # 期望布局 (1, 255, grid_h, grid_w); 若首跑 shape 不同, 按打印调整
    data = np.squeeze(output, axis=0)          # -> (255, gh, gw)
    gh, gw = data.shape[1], data.shape[2]
    data = data.reshape(3, BOX_SIZE, gh, gw)   # (3 anchor, 85, gh, gw)
    boxes, scores, clss = [], [], []
    for a in range(3):                          # anchor
        plane = data[a]                         # (85, gh, gw)
        for i in range(gh):
            for j in range(gw):
                obj = float(plane[4, i, j])
                if obj < CONF_TH:               # obj 分粗筛
                    continue
                cls_plane = plane[5:, i, j]
                cls_id = int(np.argmax(cls_plane))
                cls_p  = float(cls_plane[cls_id])
                score  = obj * cls_p
                if score < CONF_TH:
                    continue
                # YOLOv5 解码(与 yolo.cc 相同)
                tx = float(plane[0, i, j]); ty = float(plane[1, i, j])
                tw = float(plane[2, i, j]); th = float(plane[3, i, j])
                cx = (tx * 2 - 0.5 + j) * stride
                cy = (ty * 2 - 0.5 + i) * stride
                bw = (tw * 2) ** 2 * anchors[a*2]
                bh = (th * 2) ** 2 * anchors[a*2+1]
                x1, y1 = cx - bw/2, cy - bh/2
                boxes.append([x1, y1, bw, bh])
                scores.append(score)
                clss.append(cls_id)
    return boxes, scores, clss


def iou(a, b):
    ax1, ay1, aw, ah = a; bx1, by1, bw, bh = b
    x1, y1 = max(ax1, bx1), max(ay1, by1)
    x2, y2 = min(ax1+aw, bx1+bw), min(ay1+ah, by1+bh)
    inter = max(0, x2-x1) * max(0, y2-y1)
    ua = aw*ah + bw*bh - inter
    return inter / ua if ua > 0 else 0


def nms(boxes, scores, clss):
    """按类别 NMS(与官方一致), 返回保留索引"""
    keep = []
    order = sorted(range(len(scores)), key=lambda i: scores[i], reverse=True)
    while order:
        i = order.pop(0)
        keep.append(i)
        order = [j for j in order
                 if clss[j] != clss[i] or iou(boxes[i], boxes[j]) <= NMS_TH]
    return keep


def main():
    rknn_path = sys.argv[1] if len(sys.argv) > 1 else 'yolov5.rknn'
    img_path  = sys.argv[2] if len(sys.argv) > 2 else 'bus.jpg'
    out_path  = sys.argv[3] if len(sys.argv) > 3 else 'out.jpg'

    net = RKNNLite()
    net.load_rknn(rknn_path)
    net.init_runtime()
    print('[OK] rknn loaded:', rknn_path)

    img = cv2.imread(img_path)
    if img is None:
        print('[ERR] 打不开图片:', img_path); sys.exit(1)
    img_rgb = cv2.cvtColor(img, cv2.COLOR_BGR2RGB)
    img640, scale, padx, pady = letterbox(img_rgb)
    print('[OK] 输入:', img_path, img.shape, '-> letterbox 640')

    outs = net.inference(inputs=[img640[None, ...]])   # (1,640,640,3)
    print('[INFO] 输出路数:', len(outs))
    for i, o in enumerate(outs):
        print('   out%d shape=%s dtype=%s' % (i, o.shape, o.dtype))   # 首跑先看布局

    all_boxes, all_scores, all_clss = [], [], []
    for i, out in enumerate(outs):
        b, s, c = decode_one(out, STRIDES[i], ANCHORS[i])
        all_boxes += b; all_scores += s; all_clss += c
    keep = nms(all_boxes, all_scores, all_clss)

    print('==== 检出(仅 person/car) ====')
    hit = 0
    for k in keep:
        cid = all_clss[k]
        if cid not in WHITELIST:
            continue
        hit += 1
        # 640 坐标 → 原图坐标
        x1 = int((all_boxes[k][0] - padx) / scale)
        y1 = int((all_boxes[k][1] - pady) / scale)
        x2 = int((all_boxes[k][0] + all_boxes[k][2] - padx) / scale)
        y2 = int((all_boxes[k][1] + all_boxes[k][3] - pady) / scale)
        name = COCO_NAMES[cid]
        print('  %-8s @ (%d,%d,%d,%d) %.3f' % (name, x1, y1, x2, y2, all_scores[k]))
        cv2.rectangle(img, (x1, y1), (x2, y2), (0, 255, 255), 2)
        cv2.putText(img, '%s %.2f' % (name, all_scores[k]), (x1, max(0, y1-8)),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0, 0, 255), 2)
    cv2.imwrite(out_path, img)
    print('[DONE] 白名单命中 %d 个, 结果已存: %s' % (hit, out_path))


if __name__ == '__main__':
    main()
