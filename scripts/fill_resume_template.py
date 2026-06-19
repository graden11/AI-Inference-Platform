from __future__ import annotations

import shutil
import zipfile
from pathlib import Path

from lxml import etree


W_NS = "http://schemas.openxmlformats.org/wordprocessingml/2006/main"
NS = {"w": W_NS}


def own_text_nodes(paragraph):
    nodes = []
    for node in paragraph.xpath(".//w:t", namespaces=NS):
        parent = node.getparent()
        while parent is not None and parent.tag != f"{{{W_NS}}}p":
            parent = parent.getparent()
        if parent is paragraph:
            nodes.append(node)
    return nodes


def set_paragraph_text(paragraph, text: str) -> None:
    nodes = own_text_nodes(paragraph)
    if not nodes:
        return
    nodes[0].text = text
    if text.startswith(" ") or text.endswith(" "):
        nodes[0].set(f"{{http://www.w3.org/XML/1998/namespace}}space", "preserve")
    for node in nodes[1:]:
        node.text = ""


def apply_repeated(paragraphs, blocks) -> None:
    for indices, texts in blocks:
        for offset, text in enumerate(texts):
            for start in indices:
                idx = start + offset
                if idx < len(paragraphs):
                    set_paragraph_text(paragraphs[idx], text)


def main() -> None:
    src = Path(r"C:\Users\Administrator\Downloads\aaaa.docx")
    out = Path(r"D:\jetbrains\clion-project\httpserver\outputs\郭江伟_简历_CppAI推理平台.docx")
    out.parent.mkdir(parents=True, exist_ok=True)

    tmp = out.with_suffix(".tmp.docx")
    shutil.copyfile(src, tmp)

    with zipfile.ZipFile(tmp, "r") as zin:
        files = {name: zin.read(name) for name in zin.namelist()}

    root = etree.fromstring(files["word/document.xml"])
    paragraphs = root.xpath("//w:p", namespaces=NS)

    project_text = [
        "高性能 C++ AI 推理服务平台",
        "项目描述：基于 muduo Reactor 网络库构建 HTTPServer，并在其上实现支持 CPU/GPU 双后端的 AI 推理平台。",
        "技术栈：C++17、muduo、TensorRT、ONNX Runtime、CUDA、Docker、Redis、MySQL、Python 压测工具。",
        "主要工作：",
        "1、实现 HTTP 请求解析、路由分发、中间件链、会话管理、Keep-Alive、优雅关闭等通用 WebServer 能力；",
        "2、封装模型热加载、版本化管理、ONNX CPU 推理与 TensorRT FP16 GPU 推理，支持前端可视化调用；",
        "3、设计动态批处理调度器，支持 preferred batch size、max queue delay、按模型分组和异步响应回写；",
        "4、梳理请求生命周期，修复异步完成、配置持久化、批量请求、stb_image 并发解码等稳定性问题；",
        "5、建设 /metrics/json 指标与 Keep-Alive 压测脚本，形成“定位瓶颈-调参-验证”的性能优化闭环。",
        "性能成果：",
        "1、TensorRT 单模型峰值吞吐由约 28 QPS 提升至 1025 QPS，提升约 36 倍，错误率 0%；",
        "2、动态 batch 在并发 128 下平均 batch size 约 15.8，P95 延迟约 146ms，GPU 利用率显著提升。",
        "项目亮点：覆盖网络框架、HTTP 服务、AI 推理、异步调度、性能压测和工程稳定性治理。",
    ]

    advantages = [
        "1、具备 C++ 网络编程和 Linux 高并发服务开发能力，能从 Reactor、线程模型、缓冲区和请求生命周期定位问题；",
        "2、熟悉 AI 推理服务工程化，理解模型加载、批处理、前处理、TensorRT/ONNX Runtime 等关键链路；",
        "3、习惯使用 metrics、日志和压测数据驱动优化，能将性能问题拆解到网络、CPU、GPU 和调度层；",
        "4、项目覆盖底层网络库、中间 HTTPServer 与上层推理平台，具备完整系统设计和落地能力。",
    ]

    skills = [
        "核心开发能力",
        "C++ / Linux：",
        "熟悉 C++11/14/17、RAII、智能指针、多线程、线程池、异步回调、锁与生命周期管理",
        "网络与服务端：",
        "熟悉 muduo Reactor 模型、Epoll、eventfd、HTTP/1.1、Keep-Alive、路由、中间件和优雅关闭",
        "AI 推理工程：",
        "熟悉 ONNX Runtime、TensorRT、FP16、动态 batch、模型热加载、版本管理和 GPU 推理链路",
        "性能与工具：",
        "熟悉 Docker/WSL2、CMake、GDB、日志与 metrics，能够编写 Python 压测脚本进行 QPS/P95/P99 分析",
        "计算机基础：",
        "掌握操作系统、计算机网络、数据结构、数据库基础，具备端到端问题排查能力",
    ]

    apply_repeated(
        paragraphs,
        [
            ([1, 15, 29, 43], project_text),
            ([82, 86], advantages),
            ([94, 112], skills),
        ],
    )

    for idx in (57, 58):
        set_paragraph_text(paragraphs[idx], "项目经历")
    for idx in (92, 93):
        set_paragraph_text(paragraphs[idx], "专业技能")

    files["word/document.xml"] = etree.tostring(
        root,
        xml_declaration=True,
        encoding="UTF-8",
        standalone="yes",
    )

    with zipfile.ZipFile(out, "w", compression=zipfile.ZIP_DEFLATED) as zout:
        for name, data in files.items():
            zout.writestr(name, data)

    tmp.unlink(missing_ok=True)
    print(out)


if __name__ == "__main__":
    main()
