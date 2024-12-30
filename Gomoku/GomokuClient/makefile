# 编译器
CXX = g++

# 编译选项
CXXFLAGS = -std=c++17 -Wall -g

# 链接的库
LIBS = -lPocoNet -lPocoFoundation -lPocoJSON -lncursesw

# 目标文件
TARGET = client

# 源文件列表
SRCS = $(wildcard *.cpp)

# 对应的目标文件
OBJS = $(SRCS:.cpp=.o)

# 编译规则
$(TARGET): $(SRCS)
	@echo "Linking target $@"
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS)

# 清理规则
clean:
	@echo "Cleaning up..."
	@rm -f $(TARGET)

# 伪目标
.PHONY: clean
