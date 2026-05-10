#include "../../../SDK/SDK.h"
#include "../CFG.h"
#include <regex>
#include <string>

// Process incoming chat message and auto-solve math problems
void ProcessChatForMath(const wchar_t* message)
{
	if (!CFG::Misc_Chat_AutoMath_Active)
		return;

	if (!I::EngineClient->IsConnected() || !I::EngineClient->IsInGame())
		return;

	// Convert wchar_t to string for easier regex processing
	std::wstring wstr(message);
	std::string str(wstr.begin(), wstr.end());

	// Very flexible regex - just look for: number operator number = ??
	// This will match regardless of what's before/after
	std::regex mathPattern(R"((\d+)\s*([+\-*/])\s*(\d+)\s*=\s*\?\?)");
	
	std::smatch match;
	if (std::regex_search(str, match, mathPattern))
	{
		// Extract the numbers and operator
		int num1 = std::stoi(match[1].str());
		char op = match[2].str()[0];
		int num2 = std::stoi(match[3].str());
		
		// Calculate the result
		int result = 0;
		bool validOp = true;
		
		switch (op)
		{
			case '+':
				result = num1 + num2;
				break;
			case '-':
				result = num1 - num2;
				break;
			case '*':
				result = num1 * num2;
				break;
			case '/':
				if (num2 != 0)
					result = num1 / num2;
				else
					validOp = false; // Division by zero
				break;
			default:
				validOp = false;
				break;
		}
		
		if (validOp)
		{
			// Send the answer to chat
			std::string answer = std::to_string(result);
			std::string cmd = "say \"" + answer + "\"";
			I::EngineClient->ClientCmd_Unrestricted(cmd.c_str());
		}
	}
}
