## DD交易接口

### 接口介绍
本接口为顶点**DD**交易接口。

### 使用说明
1. 编译运行时需要用Release模式，最好使用64位编译。
2. 测试时Debug模式下回调结果可能是乱码，或者回调时程序中断退出。
3. 此处使用的依赖文件来自HTS接口，调用东吴a5接口的依赖文件进行查询时程序会频繁中断退出。

### 参数说明
1. config.yaml文件里node是节点号，*MAC*表示物理地址；*PC*指终端类型，Windows用设置为<u>PC</u>，Linux用户设置为<u>OH</u>；*HD*表示硬盘序列号；*LIP*表示内网IP。
