# CloverCore

一个现代，高度可扩展的，面向 QQNT 的 Native/Javascript 插件开发框架。

## 为什么选择 CloverCore

我们有前所未有的，面向原生开发者的丰富 API:

- 借助 [libcxx-build](https://github.com/CloverNT/libcxx-build) 提供的 libc++ 预构建和 [qqnt-sdk](https://github.com/CloverNT/qqnt-sdk) 提供的 node/v8 的头文件，轻松的对 node 和 v8 进行API调用，而无需担心ABI问题
- 通过使用 [Alka](https://github.com/SingerBallad/Alka) 提供的跨平台 C++/JS 绑定，轻松的在 C++ 和 JavaScript 之间进行数据传递和函数调用
- 使用 [Mortis](https://github.com/SingerBallad/Mortis) 提供的跨平台 Inline Hook/IAT Hook 和内存补丁，特征码扫描等功能，轻松的对QQ内部进行修改，挂钩等操作
- 访问项目内建的C++ <-> JS跨域事件系统，轻松的进行跨语言事件传递和处理
- 还有更多 ...

我们也面向 JavaScript 开发者提供了插件加载接口~~和丰富的API~~ (待施工ing)

## 开始使用

CloverCore 依赖 CloverLoader, 请确保你已经安装了CloverLoader。

> CloverLoader 不公开发布，请自行寻找

之后在 [Release](https://github.com/CloverNT/CloverCore/releases) 或是 [Actions](https://github.com/CloverNT/CloverCore/actions) 下载对应平台的 CloverCore，将插件放入`./plugins`目录下，启动 CloverLoader 即可。

通常来说，部署好的CloverCore的文件夹遵循如下结构:

```tree
plugins/
├─ CloverCore/
│  ├── framework/
│  │   └── ...
│  ├── clover.json
│  └── CloverCore.dll
└── ....
```

其中`clover.json`是每个插件的描述文件，对于 CloverCore 的一般形式为:

```json
{
  "name": "CloverCore",
  "type": "raw", // 特殊的类型，以后会注明
  "version": "1.0.0", // 由构建自动填入
  "author": "CloverNT",
  "description": "Modern platform and Javascript plugin loader for CloverNT.",
  "main": "CloverCore.dll" // 在Linux上会是CloverCore.so
}
```

> `clover.json`由构建时根据 `clover.json.in` 自动生成（版本号取构建版本的数字部分，没有 tag 时是 `0.0.0`），和 CloverCore 动态库一起输出到 `build/dist/CloverCore/`，CI 产物同样是 `CloverCore/` 目录，直接放入 `plugins/` 即可

在确保全部部署好后，启动 CloverLoader 即可。

## 使用 CloverCore 开发

对于原生插件的开发者，我们准备了 [NativePluginTemplate](https://github.com/CloverNT/NativePluginTemplate) 作为开发模板，您可以直接使用它来快速开始开发。

对于 Javascript 插件的开发者，我们尚未提供开发说明和模板，在不久的将来我们会提供相关的开发说明和模板，敬请期待。

### 在插件列表展示 logo

在插件根目录的 `clover.json` 中填写可选的顶层字符串 `logo`，路径相对于该目录。例如，将图片放在 `Greeting/assets/icon.png`，使用如下清单。

```json
{
  "name": "Greeting",
  "type": "js",
  "entry": "main.js",
  "logo": "assets/icon.png"
}
```

支持 PNG（.png）、JPEG（.jpg 或 .jpeg）和 WebP（.webp），每个文件最大 256 KiB（262144 字节）。图片必须是插件目录内的普通文件，扩展名须与文件签名一致。绝对路径、`..` 路径段、网络地址、data URL 和 SVG 都会被忽略；经符号链接或 Windows 目录联接指向插件目录之外的图片也会被忽略。省略字段、缺少图片或图片无效时，插件仍可正常加载，列表使用默认图标。

`logo` 只供设置窗口读取，原生插件沿用现有 `Manifest` ABI。

## 文档

尚未提供，尽请期待。

## 讨论和交流

QQ群: 1097520805

除此群外暂无其他官方讨论群。

## 许可

使用LGPL-3.0 License，详情请查看 LICENSE 文件。
