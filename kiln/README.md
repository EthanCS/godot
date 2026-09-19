# Kiln

基于 Godot 4.7.2-stable 的引擎级延迟渲染管线，上游版本固定在
[`upstream.json`](upstream.json)。Godot 原有历史与许可证保留。

当前架构、执行顺序、构建运行方式、验证范围与来源说明统一维护在
**[Kiln 渲染流程](docs/PIPELINE.md)**。

高级 GI 只在 `kiln_deferred` 中运行。Godot 原生 Forward+、Mobile、Compatibility
保持各自的渲染路径。带 NRD SDK 的 Vulkan 构建始终启用 NRD 间接漫反射与间接高光降噪。
