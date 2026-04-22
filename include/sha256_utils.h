#ifndef __SHA256_UTILS_H_
#define __SHA256_UTILS_H_

//函数作用：通过popen调用系统命令计算文件的SHA256值，并将结果输出到sha256_out中
//参数说明：
//filepath：要计算SHA256值的文件路径
//sha256_out：用于存储计算结果的字符数组，必须至少有65个字符的空间
//        （64个字符用于SHA256值，1个字符用于字符串结束符'\0'）
//返回值：成功返回0，失败返回-1

int get_file_sha256(const char *file_path, char *sha256_out);
int sha256_hash(const char *input, char *sha256_out);

#endif
