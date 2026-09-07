// 仅供 PC 端 host 单测的最小 stub：只提供 rknn_engine.hpp 声明所需的类型，
// 不链接任何 RKNN 实现。交叉编译/板上请勿使用本文件。
#pragma once

typedef void* rknn_context;

struct rknn_input_output_num {
    int n_input = 0;
    int n_output = 0;
};

struct rknn_tensor_attr {};
