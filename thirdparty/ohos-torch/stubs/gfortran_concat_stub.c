/* gfortran_concat_stub.c —— OHOS 净产物缺少的 gfortran 字符串拼接 runtime(仅个别 LAPACK
 *  例程的错误路径使用; OpenBLAS 官方链接时因宿主有 -lgfortran 而得到它)。
 *   语义与 gfortran runtime 相同(截断至 len): dst 对调 拷贝 src1|src2。
 *   2026-09-05 G4: dlopen EINVAL 的最后一个未定义符号(_gfortran_concat_string)。
 */
#include <string.h>
#include <stddef.h>
void _gfortran_concat_string (size_t *len, char *dst,
                              size_t len1, const char *src1,
                              size_t len2, const char *src2)
{
  /* gfortran 实现: dst 以 len1+len2 写入; *len 输出结果长度;
   * 与 gfortran runtime 相同,不做错位防护(调用方保证 buf 放得下)。 */
  memcpy (dst, src1, len1);
  memcpy (dst + len1, src2, len2);
  *len = len1 + len2;
}
