#include "save_name.h"
#include "encoding.h"

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
 * CJK 双字节编码字符保持不变（由当前语言配置决定）
 */
void __fastcall fonthook::SavePathProcess(void* ptThis, UInt32 EDX, char* szFileName)
{
	size_t len = CdeclCall<UInt32>(0xEC6130, szFileName);//FastStrlen
	for (size_t i = 0; i < len; i++)
	{
		UInt8 c = static_cast<UInt8>(szFileName[i]);

		// CJK 双字节编码：高位字节(>0x7F)识别并保留合法的双字节字符
		// 0-127 范围内的单字节字符始终按原逻辑处理
		if (c > 0x7F && IsLeadByte(c))
		{
			if (i + 1 < len && IsTrailByte(static_cast<UInt8>(szFileName[i + 1])))
			{
				i++;  // 跳过尾随字节，两个字节均保留
				continue;
			}
			// 尾随字节无效，按普通字符处理（替换为空格）
		}

		// 合法 ASCII 范围：'0'(48) ~ 'z'(122)
		if (c < '0' || c > 'z')
		{
			szFileName[i] = ' ';
		}
		else
		{
			// 在黑名单中的字符需要处理
			if (CharInList("\t\\/:*<>?|\"+=@^[]`;", static_cast<char>(c)))
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