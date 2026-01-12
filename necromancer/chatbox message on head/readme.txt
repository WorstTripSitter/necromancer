    #include "ChatESP.h"
    #include "../../Players/PlayerUtils.h"
     
    /*
    	this is like when MMOs have chat bubbles above heads
     
    	TODO maybe:
    	- word wrap for long msgs
    	- custom bubble shapes? rectangles look fine
    	- animate the bubble appearing? maybe later
    */
     
    void CChatESP::OnChatMessage(int iPlayerIndex, const std::string& sMessage)
    {
    	if (!Vars::Visuals::ChatESP::Enabled.Value)
    		return;
     
    	// dont add empty msgs thats stupid
    	if (sMessage.empty())
    		return;
     
    	// check if player exists
    	auto pEntity = I::ClientEntityList->GetClientEntity(iPlayerIndex);
    	if (!pEntity)
    		return;
     
    	// stack em like a schizo
    	// limit per player to avoid spam abuse
    	int iSamePlayerCount = 0;
    	for (auto it = m_vBubbles.begin(); it != m_vBubbles.end();)
    	{
    		if (it->m_iPlayerIndex == iPlayerIndex)
    		{
    			iSamePlayerCount++;
    			if (iSamePlayerCount >= 3)  // max 3 bubbles per player
    			{
    				it = m_vBubbles.erase(it);
    				continue;
    			}
    		}
    		++it;
    	}
     
    	
    	std::string sClean = CleanMessage(sMessage);
    	if (sClean.empty())
    		return;
     
    	// make the bubble struct
    	ChatBubble_t tBubble;
    	tBubble.m_iPlayerIndex = iPlayerIndex;
    	tBubble.m_sMessage = sClean;
    	tBubble.m_flCreateTime = I::GlobalVars->curtime;
    	tBubble.m_flDuration = Vars::Visuals::ChatESP::Duration.Value;
    	tBubble.m_tColor = GetBubbleColor(iPlayerIndex);
     
    	// cap total bubbles so we dont eat ram
    	while (m_vBubbles.size() >= m_iMaxBubbles)
    		m_vBubbles.pop_front();
     
    	m_vBubbles.push_back(tBubble);
    }
     
    void CChatESP::Think()
    {
    	if (!Vars::Visuals::ChatESP::Enabled.Value)
    	{
    		m_vBubbles.clear();
    		return;
    	}
     
    	// yeet expired bubbles
    	float flCurTime = I::GlobalVars->curtime;
    	for (auto it = m_vBubbles.begin(); it != m_vBubbles.end();)
    	{
    		if (flCurTime - it->m_flCreateTime > it->m_flDuration)
    			it = m_vBubbles.erase(it);
    		else
    			++it;
    	}
    }
     
    void CChatESP::Draw()
    {
    	if (!Vars::Visuals::ChatESP::Enabled.Value || m_vBubbles.empty())
    		return;
     
    	auto pLocal = H::Entities.GetLocal();
    	if (!pLocal)
    		return;
     
    	const auto& fFont = H::Fonts.GetFont(FONT_ESP);
    	
    	// gotta track offsets per player for stacking bubbles
    	std::unordered_map<int, int> mPlayerOffsets;
     
    	for (auto& tBubble : m_vBubbles)
    	{
    		// get player entity
    		auto pEntity = I::ClientEntityList->GetClientEntity(tBubble.m_iPlayerIndex);
    		if (!pEntity)
    			continue;
     
    		auto pPlayer = pEntity->As<CTFPlayer>();
    		if (!pPlayer || !pPlayer->IsAlive() || pPlayer->IsDormant())
    		{
    			// skip if player invalid BUT dont remove bubble yet
    			// they mightve just gone dormant temporarily
    			continue;
    		}
     
    		// get head position 
    		Vec3 vHeadPos = GetPlayerHeadPos(tBubble.m_iPlayerIndex);
    		if (vHeadPos.IsZero())
    			continue;
     
    		// add some height so its above their head not in their brain
            // change to whatever number u think is best, 20 was in their body
    		vHeadPos.z += 45.f;
     
    		// stack offset for multiple bubbles on same player
    		int& iOffset = mPlayerOffsets[tBubble.m_iPlayerIndex];
    		vHeadPos.z += iOffset * 18.f;  //units between each bubble
    		iOffset++;
     
    		// world to screen
    		Vec3 vScreen;
    		if (!SDK::W2S(vHeadPos, vScreen))
    			continue;
     
    		// calculate alpha based on remaining time (fade out last 20%)
    		float flTimeLeft = tBubble.m_flDuration - (I::GlobalVars->curtime - tBubble.m_flCreateTime);
    		float flFadeTime = tBubble.m_flDuration * 0.2f;
    		float flAlpha = 1.f;
    		if (flTimeLeft < flFadeTime)
    			flAlpha = flTimeLeft / flFadeTime;
     
    		// also fade based on distance cuz why not
    		float flDist = pLocal->m_vecOrigin().DistTo(pPlayer->m_vecOrigin());
    		if (flDist > 2000.f)  // fade at 2k units
    		{
    			float flDistFade = 1.f - ((flDist - 2000.f) / 1000.f);
    			flAlpha = std::min(flAlpha, std::max(0.f, flDistFade));
    		}
     
    		if (flAlpha <= 0.01f)
    			continue;
     
    		//truncate long msgs
    		std::string sDisplay = tBubble.m_sMessage;
    		int iMaxChars = Vars::Visuals::ChatESP::MaxLength.Value;
    		if (sDisplay.length() > iMaxChars)
    			sDisplay = sDisplay.substr(0, iMaxChars - 3) + "...";
     
    		// get text dimensions
    		int iTextW = 0, iTextH = 0;
    		I::MatSystemSurface->GetTextSize(fFont.m_dwFont, SDK::ConvertUtf8ToWide(sDisplay).c_str(), iTextW, iTextH);
     
    		// bubble dimensions with padding
    		int iPadX = H::Draw.Scale(8);
    		int iPadY = H::Draw.Scale(4);
    		int iBubbleW = iTextW + iPadX * 2;
    		int iBubbleH = iTextH + iPadY * 2;
    		int iBubbleX = vScreen.x - iBubbleW / 2;
    		int iBubbleY = vScreen.y - iBubbleH;
     
    		// set alpha multiplier for fading
    		I::MatSystemSurface->DrawSetAlphaMultiplier(flAlpha);
     
    		// draw background bubble (rounded-ish rectangle... ok just a rectangle lol)
    		Color_t tBgColor = Vars::Menu::Theme::Background.Value;
    		tBgColor.a = static_cast<byte>(tBgColor.a * flAlpha);
    		H::Draw.FillRect(iBubbleX, iBubbleY, iBubbleW, iBubbleH, tBgColor);
    		
    		// draw border 
    		Color_t tBorderColor = tBubble.m_tColor;
    		tBorderColor.a = static_cast<byte>(tBorderColor.a * flAlpha);
    		H::Draw.LineRect(iBubbleX, iBubbleY, iBubbleW, iBubbleH, tBorderColor);
     
    		// little triangle pointer thing below bubble pointing to player head
    		if (Vars::Visuals::ChatESP::ShowPointer.Value)
    		{
    			int iTriSize = H::Draw.Scale(6);
    			int iTriX = vScreen.x;
    			int iTriY = iBubbleY + iBubbleH;
    			
    			// ghetto triangle via lines lmao
    			H::Draw.Line(iTriX - iTriSize, iTriY, iTriX, iTriY + iTriSize, tBorderColor);
    			H::Draw.Line(iTriX + iTriSize, iTriY, iTriX, iTriY + iTriSize, tBorderColor);
    			H::Draw.Line(iTriX - iTriSize, iTriY, iTriX + iTriSize, iTriY, tBgColor);  // cover border
    		}
     
    		// draw the actual text finally
    		Color_t tTextColor = Vars::Menu::Theme::Active.Value;
    		tTextColor.a = static_cast<byte>(255 * flAlpha);
    		H::Draw.String(fFont, iBubbleX + iPadX, iBubbleY + iPadY, tTextColor, ALIGN_TOPLEFT, sDisplay.c_str());
     
    		// reset alpha
    		I::MatSystemSurface->DrawSetAlphaMultiplier(1.f);
    	}
    }
     
    std::string CChatESP::CleanMessage(const std::string& sRaw)
    {
    	std::string sOut;
    	sOut.reserve(sRaw.length());
     
    	for (size_t i = 0; i < sRaw.length(); i++)
    	{
    		unsigned char c = sRaw[i];
    		
    		// skip control chars
    		if (c >= 0x01 && c <= 0x06)
    			continue;
    		
    		// skip \x07 + 6 hex chars (RGB color)
    		if (c == 0x07 && i + 6 < sRaw.length())
    		{
    			i += 6;
    			continue;
    		}
    		
    		// skip \x08 + 8 hex chars (RGBA color) 
    		if (c == 0x08 && i + 8 < sRaw.length())
    		{
    			i += 8;
    			continue;
    		}
     
    		// normal char, keep it
    		sOut += c;
    	}
     
    	// trim leading/trailing whitespace cuz people type weird
    	size_t start = sOut.find_first_not_of(" \t\n\r");
    	if (start == std::string::npos)
    		return "";
    	size_t end = sOut.find_last_not_of(" \t\n\r");
    	
    	return sOut.substr(start, end - start + 1);
    }
     
    Vec3 CChatESP::GetPlayerHeadPos(int iPlayerIndex)
    {
    	auto pEntity = I::ClientEntityList->GetClientEntity(iPlayerIndex);
    	if (!pEntity)
    		return {};
     
    	auto pPlayer = pEntity->As<CTFPlayer>();
    	if (!pPlayer)
    		return {};
     
    	// try to get actual head bone position
    	matrix3x4 aBones[MAXSTUDIOBONES];
    	if (!pPlayer->SetupBones(aBones, MAXSTUDIOBONES, BONE_USED_BY_HITBOX, I::GlobalVars->curtime))
    		return pPlayer->m_vecOrigin() + Vec3(0, 0, 72);  // fallback to origin + height
     
    	int iHeadBone = pPlayer->GetBaseToHitbox(HITBOX_HEAD);
    	if (iHeadBone == -1)
    		return pPlayer->m_vecOrigin() + Vec3(0, 0, 72);
     
    	Vec3 vHead;
    	Math::GetMatrixOrigin(aBones[iHeadBone], vHead);
    	return vHead;
    }
     
    Color_t CChatESP::GetBubbleColor(int iPlayerIndex)
    {
    	auto pLocal = H::Entities.GetLocal();
    	if (!pLocal)
    		return Vars::Menu::Theme::Accent.Value;
     
    	auto pEntity = I::ClientEntityList->GetClientEntity(iPlayerIndex);
    	if (!pEntity)
    		return Vars::Menu::Theme::Accent.Value;
     
    	auto pPlayer = pEntity->As<CTFPlayer>();
    	if (!pPlayer)
    		return Vars::Menu::Theme::Accent.Value;
     
    	// different color modes
    	switch (Vars::Visuals::ChatESP::ColorMode.Value)
    	{
    	case Vars::Visuals::ChatESP::ColorModeEnum::Custom:
    		return Vars::Visuals::ChatESP::BubbleColor.Value;
    	
    	case Vars::Visuals::ChatESP::ColorModeEnum::Team:
    	{
    		// local player always special
    		if (iPlayerIndex == I::EngineClient->GetLocalPlayer())
    			return Vars::Colors::Local.Value.a ? Vars::Colors::Local.Value : Color_t(100, 200, 255);
    		
    		// friend check
    		if (H::Entities.IsFriend(iPlayerIndex))
    			return F::PlayerUtils.m_vTags[F::PlayerUtils.TagToIndex(FRIEND_TAG)].m_tColor;
    		
    		// team colors
    		bool bSameTeam = pPlayer->m_iTeamNum() == pLocal->m_iTeamNum();
    		if (bSameTeam)
    			return Color_t(150, 200, 255);  // blueish for team
    		else
    			return Color_t(255, 150, 150);  // reddish for enemy
    	}
    	
    	case Vars::Visuals::ChatESP::ColorModeEnum::Health:
    	{
    		float flHealth = pPlayer->m_iHealth();
    		float flMaxHealth = pPlayer->GetMaxHealth();
    		float flRatio = std::clamp(flHealth / flMaxHealth, 0.f, 1.f);
    		return Vars::Colors::IndicatorBad.Value.Lerp(Vars::Colors::IndicatorGood.Value, flRatio);
    	}
    	
    	default:
    		return Vars::Menu::Theme::Accent.Value;
    	}
    }



    struct ChatBubble_t
    {
    	int m_iPlayerIndex = 0;       // who said it
    	std::string m_sMessage;        // what they said (cleaned)
    	float m_flCreateTime = 0.f;   // when they said it
    	float m_flDuration = 5.f;     // how long to show
    	Color_t m_tColor;             // msg color (team based or custom idc)
    };
     
    class CChatESP
    {
    public:
    	void OnChatMessage(int iPlayerIndex, const std::string& sMessage);
    	void Draw();
    	void Think();  // cleanup old msgs
     
    private:
    	std::deque<ChatBubble_t> m_vBubbles;
    	size_t m_iMaxBubbles = 32;  // dont want infinite memory usage lmao
     
    	// helper funcs
    	std::string CleanMessage(const std::string& sRaw);  // strips color codes n [removed]
    	Vec3 GetPlayerHeadPos(int iPlayerIndex);
    	Color_t GetBubbleColor(int iPlayerIndex);
    };
     
    ADD_FEATURE(CChatESP, ChatESP);