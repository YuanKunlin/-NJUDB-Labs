# NJUDB 实验归档（Lab01–Lab03）

本仓库是南京大学《数据库系统内核实验》的完成记录，基线为课程仓库
[nju-websoft/NJU_DBPractice](https://github.com/nju-websoft/NJU_DBPractice) 的提交 `0cf929a`。

## 分支说明

| 分支 | 内容 | 编译配置 | 官方评测 |
| --- | --- | --- | --- |
| `lab01` | 实验一 存储管理：t1 LRU、t2 缓冲池、t3 表句柄、f1 LRU-K、f2 PAX | `SOURCE_LABS="01"` | ctest 22/23（余下为后续实验语义） |
| `lab02` | 实验二 执行引擎（上）：t1 八个基本算子、f1 外部归并排序 + 嵌套循环内连接 | `SOURCE_LABS="01 02"` | t1 5/5、f1 5/5 |
| `lab03` | 实验三 执行引擎（下）：t1 内连接、t2 左外连接、t3 聚合、f1 哈希连接、f2 排序合并连接 | `SOURCE_LABS="01 02 03"` | basic 5/5、bonus 5/5 |
| `main` | 与 `lab03` 相同的代码 + `reports/` 下的三份实验报告 | 同上 | 同上 |

`main` 是线性开发历史：每个实验题一个提交，提交信息里记录了实现要点和踩过的坑；三个 `labXX`
分支分别是该实验完成时的代码快照。

## 构建与测试

```bash
./configure.sh && cmake --build build -j
bash test/sql/evaluate.sh build lab02 t1      # lab/case 按需替换
bash test/sql/evaluate.sh build lab03 basic
bash test/sql/evaluate.sh build lab03 bonus
```

`test/sql/lab02/f1/expected/` 由同目录的 `expected.tar.gz` 解压得到（约 270MB），不入库；
`build/` 等构建产物已在 `.gitignore` 中排除。

## 环境

WSL2 + Ubuntu 24.04，g++ 13.3、CMake 3.28、flex 2.6.4、bison 3.8.2，Debug 构建。

第三方依赖（fmt 11.0.2、googletest、argparse）按锁定版本随仓库保存，克隆后无需再拉 submodule；
若需要重新同步，上游 `.gitmodules` 里有对应地址。

## 报告

`reports/lab1_241220083_袁琨林.md`、`reports/lab2_241220083_袁琨林.md`、`reports/lab3_241220083_袁琨林.md`
