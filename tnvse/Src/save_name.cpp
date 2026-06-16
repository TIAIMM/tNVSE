#include "save_name.h"

/**
 * 判断字符 c 是否在字符串 list 中（遇到 '\0' 提前结束）
 * @param list 以 '\0' 结尾的字符串（非法字符列表）
 * @param c    待检测的字符
 * @return     true: 在列表中; false: 不在或 list 已结束
 */
bool __fastcall fonthook::CharInList(const char* list, char c)
{
	while (*list)
	{
		if (*list == c)
			return true;
		list++;
	}
	return false;
}

/**
 * 对文件名进行安全清洗（原地修改）
 * 保留：0-9, A-Z, a-z 及少数允许的符号
 * 双引号 " 特殊处理为单引号 '
 * 其余非法字符（包括控制字符、高位字符、禁用符号）替换为空格
 */
void __fastcall fonthook::SavePathProcess(UInt32 a1, UInt32 a2, char* szFileName)
{
	size_t len = CdeclCall<UInt32>(0xEC6130, szFileName);//FastStrlen
	for (size_t i = 0; i < len; i++)
	{
	    char c = szFileName[i];

	    // 合法 ASCII 范围：'0'(48) ~ 'z'(122)
	    if (c < '0' || c > 'z')
	    {
	        szFileName[i] = ' ';
	    }
	    else
	    {
	        // 在黑名单中的字符需要处理
	        if (CharInList("\t\\/:*<>?|\"+=@^[]`;", c))
	        {
	            if (c == '"')
	            {
	                szFileName[i] = '\'';   // 双引号特殊处理
	            }
	            else
	            {
	                szFileName[i] = ' ';    // 其他非法字符替换为空格
	            }
	        }
	        // 不在黑名单的字符保持原值
	    }
	}
}


void fonthook::InitSaveNameHook()
{
	WriteRelCall(0x8518BB, &fonthook::SavePathProcess);
}