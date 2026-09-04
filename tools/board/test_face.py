#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# scrfd 单图测试：python3 test_face.py face.jpg out.jpg
# 说明：直接复用 11_facedet_scrfd_npu/main.py 里的 SCRFD 类（import 不触发摄像头主循环）
# 用法（板端 /userdata/aidemo/11_facedet_scrfd_npu 下）：
#   python3 test_face.py face.jpg out.jpg
import sys
import cv2
from main import SCRFD  # 复用 main.py 里已定义好的 SCRFD 类

if __name__ == '__main__':
    in_path  = sys.argv[1] if len(sys.argv) > 1 else 'face.jpg'
    out_path = sys.argv[2] if len(sys.argv) > 2 else 'out.jpg'

    mynet = SCRFD()                     # 加载 scrfd.rknn + 初始化 runtime
    img = cv2.imread(in_path)
    if img is None:
        print('[ERR] 打不开图片:', in_path)
        sys.exit(1)
    print('[OK] 输入图片:', in_path, img.shape)

    out = mynet.detect(img)             # 检测 + 画框（结果画在原图上）
    cv2.imwrite(out_path, out)
    print('[OK] 检测完成，结果已保存:', out_path)
