# WebServer-master
【代码随想录知识星球】项目分享-webserver
## 项目介绍
本项目是基于muduo网络库开发的轻量级web服务器，实现了登录、注册、页面跳转等简单功能。  
## 环境
建议使用Ubuntu相关环境，需要安装一下g++、cmake、make、boost库和muduo网络库(注意：本项目的muduo库文件和boost库文件都已安装在系统目录下)。  
基础的编译环境配置，可通过linux命令安装，在需要时自行上网搜索相应命令即可。例如：  
```sh
sudo apt install g++ cmake make
```  
boost库和muduo网络库的安装教程链接如下：  
boost库：https://blog.csdn.net/QIANGWEIYUAN/article/details/88792874  
muduo库：https://blog.csdn.net/QIANGWEIYUAN/article/details/89023980  
## 编译
当环境都配置好之后，编译生成可执行文件：  
```
make
```  
删除编译生成的可执行文件：  
```
make clean
```  
## 运行
```
./server 8888
```  



