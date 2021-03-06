/** 
 * @file llnearbychatbar.cpp
 * @brief LLNearbyChatBar class implementation
 *
 * $LicenseInfo:firstyear=2002&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
 * 
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * 
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * 
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "message.h"

#include "llappviewer.h"
#include "llfloaterreg.h"
#include "lltrans.h"

#include "llfirstuse.h"
#include "llnearbychatbar.h"
#include "llbottomtray.h"
#include "llagent.h"
#include "llgesturemgr.h"
#include "llmultigesture.h"
#include "llkeyboard.h"
#include "llanimationstates.h"
#include "llviewerstats.h"
#include "llcommandhandler.h"
#include "llviewercontrol.h"
#include "llnavigationbar.h"
#include "llwindow.h"
#include "llviewerwindow.h"
#include "llrootview.h"
// [RLVa:KB] - Checked: 2010-02-27 (RLVa-1.2.0b)
#include "rlvhandler.h"
// [/RLVa:KB]
#include "chatbar_as_cmdline.h"

S32 LLNearbyChatBar::sLastSpecialChatChannel = 0;

// legacy callback glue
//void send_chat_from_viewer(const std::string& utf8_out_text, EChatType type, S32 channel);
// [RLVa:KB] - Checked: 2010-02-27 (RLVa-1.2.0b) | Modified: RLVa-0.2.2a
void send_chat_from_viewer(std::string utf8_out_text, EChatType type, S32 channel);
// [/RLVa:KB]

static LLDefaultChildRegistry::Register<LLGestureComboList> r("gesture_combo_list");

struct LLChatTypeTrigger {
	std::string name;
	EChatType type;
};

static LLChatTypeTrigger sChatTypeTriggers[] = {
	{ "/whisper"	, CHAT_TYPE_WHISPER},
	{ "/shout"	, CHAT_TYPE_SHOUT}
};

//ext-7367
//Problem: gesture list control (actually LLScrollListCtrl) didn't actually process mouse wheel message. 
// introduce new gesture list subclass to "eat" mouse wheel messages (and probably some other messages)
class LLGestureScrollListCtrl: public LLScrollListCtrl
{
protected:
	friend class LLUICtrlFactory;
	LLGestureScrollListCtrl(const LLScrollListCtrl::Params& params)
		:LLScrollListCtrl(params)
	{
	}
public:
	BOOL handleScrollWheel(S32 x, S32 y, S32 clicks)
	{
		LLScrollListCtrl::handleScrollWheel( x, y, clicks );
		return TRUE;
	}
	//See EXT-6598
	//Mouse hover over separator will result in not processing tooltip message
	//So eat this message
	BOOL handleToolTip(S32 x, S32 y, MASK mask)
	{
		LLScrollListCtrl::handleToolTip( x, y, mask );
		return TRUE;
	}
};

LLGestureComboList::Params::Params()
:	combo_button("combo_button"),
	combo_list("combo_list"),
	get_more("get_more", true),
	view_all("view_all", true)
{
}

LLGestureComboList::LLGestureComboList(const LLGestureComboList::Params& p)
:	LLUICtrl(p),
	mLabel(p.label),
	mViewAllItemIndex(-1),
	mGetMoreItemIndex(-1),
	mShowViewAll(p.view_all),
	mShowGetMore(p.get_more)
{
	LLBottomtrayButton::Params button_params = p.combo_button;
	button_params.follows.flags(FOLLOWS_LEFT|FOLLOWS_BOTTOM|FOLLOWS_RIGHT);

	mButton = LLUICtrlFactory::create<LLBottomtrayButton>(button_params);
	mButton->reshape(getRect().getWidth(),getRect().getHeight());
	mButton->setCommitCallback(boost::bind(&LLGestureComboList::onButtonCommit, this));

	addChild(mButton);

	LLGestureScrollListCtrl::Params params(p.combo_list);
	
	params.name("GestureComboList");
	params.commit_callback.function(boost::bind(&LLGestureComboList::onItemSelected, this, _2));
	params.visible(false);
	params.commit_on_keyboard_movement(false);

	mList = LLUICtrlFactory::create<LLGestureScrollListCtrl>(params);
	addChild(mList);

	//****************************Gesture Part********************************/

	setCommitCallback(boost::bind(&LLGestureComboList::onCommitGesture, this));

	// now register us as observer since we have a place to put the results
	LLGestureMgr::instance().addObserver(this);

	// refresh list from current active gestures
	refreshGestures();

	setFocusLostCallback(boost::bind(&LLGestureComboList::hideList, this));
}

BOOL LLGestureComboList::handleKeyHere(KEY key, MASK mask)
{
	BOOL handled = FALSE;
	
	if (key == KEY_ESCAPE && mask == MASK_NONE )
	{
		hideList();
		handled = TRUE;
	}
	else
	{
		handled = mList->handleKeyHere(key, mask);
	}

	return handled; 		
}

void LLGestureComboList::showList()
{
	LLRect rect = mList->getRect();
	LLRect button_rect = mButton->getRect();
	
	// Calculating amount of space between the navigation bar and gestures combo
	LLNavigationBar* nb = LLNavigationBar::getInstance();

	S32 x, nb_bottom;
	nb->localPointToOtherView(0, 0, &x, &nb_bottom, this);

	S32 max_height = nb_bottom - button_rect.mTop;
	mList->calcColumnWidths();
	rect.setOriginAndSize(button_rect.mLeft, button_rect.mTop, llmax(mList->getMaxContentWidth(),mButton->getRect().getWidth()), max_height);

	mList->setRect(rect);
	mList->fitContents( llmax(mList->getMaxContentWidth(),mButton->getRect().getWidth()), max_height);

	gFocusMgr.setKeyboardFocus(this);

	// Show the list and push the button down
	mButton->setToggleState(TRUE);
	mList->setVisible(TRUE);
	LLUI::addPopup(mList);
}

void LLGestureComboList::onButtonCommit()
{
	if (!mList->getVisible())
	{
		// highlight the last selected item from the original selection before potentially selecting a new item
		// as visual cue to original value of combo box
		LLScrollListItem* last_selected_item = mList->getLastSelectedItem();
		if (last_selected_item)
		{
			mList->mouseOverHighlightNthItem(mList->getItemIndex(last_selected_item));
		}

		if (mList->getItemCount() != 0)
		{
			showList();
		}
	}
	else
	{
		hideList();
	} 
}

void LLGestureComboList::hideList()
{
	if (mList->getVisible())
	{
		mButton->setToggleState(FALSE);
		mList->setVisible(FALSE);
		mList->mouseOverHighlightNthItem(-1);
		LLUI::removePopup(mList);
		gFocusMgr.setKeyboardFocus(NULL);
	}
}

S32 LLGestureComboList::getCurrentIndex() const
{
	LLScrollListItem* item = mList->getFirstSelected();
	if( item )
	{
		return mList->getItemIndex( item );
	}
	return -1;
}

void LLGestureComboList::onItemSelected(const LLSD& data)
{
	const std::string name = mList->getSelectedItemLabel();

	S32 cur_id = getCurrentIndex();
	mLastSelectedIndex = cur_id;
	if (cur_id != mList->getItemCount()-1 && cur_id != -1)
	{
		mButton->setLabel(name);
	}

	// hiding the list reasserts the old value stored in the text editor/dropdown button
	hideList();

	// commit does the reverse, asserting the value in the list
	onCommit();
}

void LLGestureComboList::sortByName(bool ascending)
{
	mList->sortOnce(0, ascending);
}

LLSD LLGestureComboList::getValue() const
{
	LLScrollListItem* item = mList->getFirstSelected();
	if( item )
	{
		return item->getValue();
	}
	else
	{
		return LLSD();
	}
}

void LLGestureComboList::refreshGestures()
{
	//store current selection so we can maintain it
	LLSD cur_gesture = getValue();
	
	mList->selectFirstItem();
	mList->clearRows();
	mGestures.clear();

	LLGestureMgr::item_map_t::const_iterator it;
	const LLGestureMgr::item_map_t& active_gestures = LLGestureMgr::instance().getActiveGestures();
	LLSD::Integer idx(0);
	for (it = active_gestures.begin(); it != active_gestures.end(); ++it)
	{
		LLMultiGesture* gesture = (*it).second;
		if (gesture)
		{
			mList->addSimpleElement(gesture->mName, ADD_BOTTOM, LLSD(idx));
			mGestures.push_back(gesture);
			idx++;
		}
	}

	sortByName();

	// store indices for Get More and View All items (idx is the index followed by the last added Gesture)
	if (mShowGetMore)
	{
		mGetMoreItemIndex = idx;
		mList->addSimpleElement(LLTrans::getString("GetMoreGestures"), ADD_BOTTOM, LLSD(mGetMoreItemIndex));
	}
	if (mShowViewAll)
	{
		mViewAllItemIndex = idx + 1;
		mList->addSimpleElement(LLTrans::getString("ViewAllGestures"), ADD_BOTTOM, LLSD(mViewAllItemIndex));
	}

	// Insert label after sorting, at top, with separator below it
	mList->addSeparator(ADD_TOP);	
	mList->addSimpleElement(mLabel, ADD_TOP);

	if (cur_gesture.isDefined())
	{ 
		mList->selectByValue(cur_gesture);

	}
	else
	{
		mList->selectFirstItem();
	}

	LLCtrlListInterface* gestures = getListInterface();
	LLMultiGesture* gesture = NULL;
	
	if (gestures)
	{
		S32 sel_index = gestures->getFirstSelectedIndex();
		if (sel_index != 0)
		{
			S32 index = gestures->getSelectedValue().asInteger();
			if (index<0 || index >= (S32)mGestures.size())
			{
				llwarns << "out of range gesture access" << llendl;
			}
			else
			{
				gesture = mGestures.at(index);
			}
		}
	}
	
	if(gesture && LLGestureMgr::instance().isGesturePlaying(gesture))
	{
		return;
	}
	
	mButton->setLabel(mLabel);
}

void LLGestureComboList::onCommitGesture()
{
	LLCtrlListInterface* gestures = getListInterface();
	if (gestures)
	{
		S32 sel_index = gestures->getFirstSelectedIndex();
		if (sel_index == 0)
		{
			return;
		}

		S32 index = gestures->getSelectedValue().asInteger();

		if (mViewAllItemIndex == index)
		{
			// The same behavior as Ctrl+G. EXT-823
			LLFloaterReg::toggleInstance("gestures");
			gestures->selectFirstItem();
			return;
		}

		if (mGetMoreItemIndex == index)
		{
			LLWeb::loadURLExternal(gSavedSettings.getString("GesturesMarketplaceURL"));
			return;
		}

		if (index<0 || index >= (S32)mGestures.size())
		{
			llwarns << "out of range gesture index" << llendl;
		}
		else
		{
			LLMultiGesture* gesture = mGestures.at(index);
			if(gesture)
			{
				LLGestureMgr::instance().playGesture(gesture);
				if(!gesture->mReplaceText.empty())
				{
					LLNearbyChatBar::sendChatFromViewer(gesture->mReplaceText, CHAT_TYPE_NORMAL, FALSE);
				}
			}
		}
	}
}

LLGestureComboList::~LLGestureComboList()
{
	LLGestureMgr::instance().removeObserver(this);
}

LLCtrlListInterface* LLGestureComboList::getListInterface()
{
	return mList;
}

LLNearbyChatBar::LLNearbyChatBar() 
:	mChatBox(NULL)
{
	mSpeakerMgr = LLLocalSpeakerMgr::getInstance();
}

//virtual
BOOL LLNearbyChatBar::postBuild()
{
	mChatBox = getChild<LLLineEditor>("chat_box");

	mChatBox->setCommitCallback(boost::bind(&LLNearbyChatBar::onChatBoxCommit, this));
	mChatBox->setKeystrokeCallback(&onChatBoxKeystroke, this);
	mChatBox->setFocusLostCallback(boost::bind(&onChatBoxFocusLost, _1, this));
	mChatBox->setFocusReceivedCallback(boost::bind(&LLNearbyChatBar::onChatBoxFocusReceived, this));

	mChatBox->setIgnoreArrowKeys( FALSE ); 
	mChatBox->setCommitOnFocusLost( FALSE );
	mChatBox->setRevertOnEsc( FALSE );
	mChatBox->setIgnoreTab(TRUE);
	mChatBox->setPassDelete(TRUE);
	mChatBox->setReplaceNewlinesWithSpaces(FALSE);
	mChatBox->setEnableLineHistory(TRUE);

	mOutputMonitor = getChild<LLOutputMonitorCtrl>("chat_zone_indicator");
	mOutputMonitor->setVisible(FALSE);

	PhoenixPlayChatAnimation = gSavedSettings.getBOOL("PhoenixPlayChatAnimation");
	gSavedSettings.getControl("PhoenixPlayChatAnimation")->getSignal()->connect(boost::bind(&LLNearbyChatBar::updatePhoenixPlayChatAnimation, this, _2));

	return TRUE;
}

//static
LLNearbyChatBar* LLNearbyChatBar::getInstance()
{
	return LLBottomTray::getInstance() ? LLBottomTray::getInstance()->getNearbyChatBar() : NULL;
}

//static
bool LLNearbyChatBar::instanceExists()
{
	return LLBottomTray::instanceExists() && LLBottomTray::getInstance()->getNearbyChatBar() != NULL;
}

void LLNearbyChatBar::draw()
{
	displaySpeakingIndicator();
	LLPanel::draw();
}

std::string LLNearbyChatBar::getCurrentChat()
{
	return mChatBox ? mChatBox->getText() : LLStringUtil::null;
}

// virtual
BOOL LLNearbyChatBar::handleKeyHere( KEY key, MASK mask )
{
	BOOL handled = FALSE;

	if( KEY_RETURN == key )
	{
		if (mask == MASK_CONTROL)
		{
			// shout
			sendChat(CHAT_TYPE_SHOUT);
			handled = TRUE;
		}
		else if (mask == MASK_SHIFT)
		{
			// whisper
			sendChat(CHAT_TYPE_WHISPER);
			handled = TRUE;
		}
		else if (mask == MASK_ALT)
		{
			// OOC
			sendChat(CHAT_TYPE_OOC);
			handled = TRUE;
		}
		else if (mask == MASK_NONE)
		{
			// say
			sendChat( CHAT_TYPE_NORMAL );
			handled = TRUE;
		}
	}

	return handled;
}

BOOL LLNearbyChatBar::matchChatTypeTrigger(const std::string& in_str, std::string* out_str)
{
	U32 in_len = in_str.length();
	S32 cnt = sizeof(sChatTypeTriggers) / sizeof(*sChatTypeTriggers);
	
	for (S32 n = 0; n < cnt; n++)
	{
		if (in_len > sChatTypeTriggers[n].name.length())
			continue;

		std::string trigger_trunc = sChatTypeTriggers[n].name;
		LLStringUtil::truncate(trigger_trunc, in_len);

		if (!LLStringUtil::compareInsensitive(in_str, trigger_trunc))
		{
			*out_str = sChatTypeTriggers[n].name;
			return TRUE;
		}
	}

	return FALSE;
}

void LLNearbyChatBar::onChatBoxKeystroke(LLLineEditor* caller, void* userdata)
{
	LLFirstUse::otherAvatarChatFirst(false);

	LLNearbyChatBar* self = (LLNearbyChatBar *)userdata;

	LLWString raw_text = self->mChatBox->getWText();

	// Can't trim the end, because that will cause autocompletion
	// to eat trailing spaces that might be part of a gesture.
	LLWStringUtil::trimHead(raw_text);

	S32 length = raw_text.length();

//	if( (length > 0) && (raw_text[0] != '/') )  // forward slash is used for escape (eg. emote) sequences
// [RLVa:KB] - Checked: 2010-03-26 (RLVa-1.2.0b) | Modified: RLVa-1.0.0d
	if ( (length > 0) && (raw_text[0] != '/') && (!gRlvHandler.hasBehaviour(RLV_BHVR_REDIRCHAT)) )
// [/RLVa:KB]
	{
		gAgent.startTyping();
	}
	else
	{
		gAgent.stopTyping();
	}

	/* Doesn't work -- can't tell the difference between a backspace
	   that killed the selection vs. backspace at the end of line.
	if (length > 1 
		&& text[0] == '/'
		&& key == KEY_BACKSPACE)
	{
		// the selection will already be deleted, but we need to trim
		// off the character before
		std::string new_text = raw_text.substr(0, length-1);
		self->mInputEditor->setText( new_text );
		self->mInputEditor->setCursorToEnd();
		length = length - 1;
	}
	*/

	KEY key = gKeyboard->currentKey();

	// Ignore "special" keys, like backspace, arrows, etc.
	if (length > 1 
		&& raw_text[0] == '/'
		&& key < KEY_SPECIAL)
	{
		// we're starting a gesture, attempt to autocomplete

		std::string utf8_trigger = wstring_to_utf8str(raw_text);
		std::string utf8_out_str(utf8_trigger);

		if (LLGestureMgr::instance().matchPrefix(utf8_trigger, &utf8_out_str))
		{
			std::string rest_of_match = utf8_out_str.substr(utf8_trigger.size());
			self->mChatBox->setText(utf8_trigger + rest_of_match); // keep original capitalization for user-entered part
			S32 outlength = self->mChatBox->getLength(); // in characters

			// Select to end of line, starting from the character
			// after the last one the user typed.
			self->mChatBox->setSelection(length, outlength);
		}
		else if (matchChatTypeTrigger(utf8_trigger, &utf8_out_str))
		{
			std::string rest_of_match = utf8_out_str.substr(utf8_trigger.size());
			self->mChatBox->setText(utf8_trigger + rest_of_match + " "); // keep original capitalization for user-entered part
			self->mChatBox->setCursorToEnd();
		}

		//llinfos << "GESTUREDEBUG " << trigger 
		//	<< " len " << length
		//	<< " outlen " << out_str.getLength()
		//	<< llendl;
	}
}

// static
void LLNearbyChatBar::onChatBoxFocusLost(LLFocusableElement* caller, void* userdata)
{
	// stop typing animation
	gAgent.stopTyping();
}

void LLNearbyChatBar::onChatBoxFocusReceived()
{
	mChatBox->setEnabled(!gDisconnected);
}

EChatType LLNearbyChatBar::processChatTypeTriggers(EChatType type, std::string &str)
{
	U32 length = str.length();
	S32 cnt = sizeof(sChatTypeTriggers) / sizeof(*sChatTypeTriggers);
	
	for (S32 n = 0; n < cnt; n++)
	{
		if (length >= sChatTypeTriggers[n].name.length())
		{
			std::string trigger = str.substr(0, sChatTypeTriggers[n].name.length());

			if (!LLStringUtil::compareInsensitive(trigger, sChatTypeTriggers[n].name))
			{
				U32 trigger_length = sChatTypeTriggers[n].name.length();

				// It's to remove space after trigger name
				if (length > trigger_length && str[trigger_length] == ' ')
					trigger_length++;

				str = str.substr(trigger_length, length);

				if (CHAT_TYPE_NORMAL == type)
					return sChatTypeTriggers[n].type;
				else
					break;
			}
		}
	}

	return type;
}

void LLNearbyChatBar::updatePhoenixPlayChatAnimation(const LLSD &data)
{
	PhoenixPlayChatAnimation = data.asBoolean();
}

void LLNearbyChatBar::sendChat( EChatType type )
{
	if (mChatBox)
	{
		LLWString text = mChatBox->getConvertedText();
		if (!text.empty())
		{
			if(type == CHAT_TYPE_OOC)
			{
				std::string tempText = mChatBox->getText();
				tempText = gSavedSettings.getString("PhoenixOOCPrefix") + " " + tempText + " " + gSavedSettings.getString("PhoenixOOCPostfix");
				mChatBox->setText(tempText);
				text = utf8str_to_wstring(tempText);
			}

			// store sent line in history, duplicates will get filtered
			mChatBox->updateHistory();
			// Check if this is destined for another channel
			S32 channel = 0;
			stripChannelNumber(text, &channel);
			
			std::string utf8text = wstring_to_utf8str(text);
			// Try to trigger a gesture, if not chat to a script.
			std::string utf8_revised_text;
			if (0 == channel)
			{
//-TT Satomi Ahn - Patch MU_OOC	
				if (gSavedSettings.getBOOL("AutoCloseOOC"))
				{
					// Try to find any unclosed OOC chat (i.e. an opening
					// double parenthesis without a matching closing double
					// parenthesis.
					if (utf8text.find("(( ") != -1 && utf8text.find("))") == -1)
					{
						// add the missing closing double parenthesis.
						utf8text += " ))";
					}
					else if (utf8text.find("((") != -1 && utf8text.find("))") == -1)
					{
						if (utf8text.at(utf8text.length() - 1) == ')')
						{
							// cosmetic: add a space first to avoid a closing triple parenthesis
							utf8text += " ";
						}
						// add the missing closing double parenthesis.
						utf8text += "))";
					}
					else if (utf8text.find("[[ ") != -1 && utf8text.find("]]") == -1)
					{
						// add the missing closing double parenthesis.
						utf8text += " ]]";
					}
					else if (utf8text.find("[[") != -1 && utf8text.find("]]") == -1)
					{
						if (utf8text.at(utf8text.length() - 1) == ']')
						{
							// cosmetic: add a space first to avoid a closing triple parenthesis
							utf8text += " ";
						}
						// add the missing closing double parenthesis.
						utf8text += "]]";
					}
				}

				// Convert MU*s style poses into IRC emotes here.
				if (gSavedSettings.getBOOL("AllowMUpose") && utf8text.find(":") == 0 && utf8text.length() > 3)
				{
					if (utf8text.find(":'") == 0)
					{
						utf8text.replace(0, 1, "/me");
	 				}
					else if (isalpha(utf8text.at(1)))	// Do not prevent smileys and such.
					{
						utf8text.replace(0, 1, "/me ");
					}
				}				
//-TT Satomi Ahn - Patch MU_OOC	
				// discard returned "found" boolean
				LLGestureMgr::instance().triggerAndReviseString(utf8text, &utf8_revised_text);
			}
			else
			{
				utf8_revised_text = utf8text;
			}

			utf8_revised_text = utf8str_trim(utf8_revised_text);

			EChatType nType;
			if(type == CHAT_TYPE_OOC)
				nType = CHAT_TYPE_NORMAL;
			else
				nType = type;

			type = processChatTypeTriggers(nType, utf8_revised_text);

			if (!utf8_revised_text.empty() && cmd_line_chat(utf8_revised_text, type))
			{
				// Chat with animation
				sendChatFromViewer(utf8_revised_text, type, PhoenixPlayChatAnimation);
			}
		}

		mChatBox->setText(LLStringExplicit(""));
	}

	gAgent.stopTyping();

	// If the user wants to stop chatting on hitting return, lose focus
	// and go out of chat mode.
	if (gSavedSettings.getBOOL("CloseChatOnReturn"))
	{
		stopChat();
	}
}

void LLNearbyChatBar::onChatBoxCommit()
{
	if (mChatBox->getText().length() > 0)
	{
		sendChat(CHAT_TYPE_NORMAL);
	}

	gAgent.stopTyping();
}

void LLNearbyChatBar::setText(const LLStringExplicit &new_text)
{
	mChatBox->setText(new_text);
}

void LLNearbyChatBar::displaySpeakingIndicator()
{
	LLSpeakerMgr::speaker_list_t speaker_list;
	LLUUID id;

	id.setNull();
	mSpeakerMgr->update(TRUE);
	mSpeakerMgr->getSpeakerList(&speaker_list, FALSE);

	for (LLSpeakerMgr::speaker_list_t::iterator i = speaker_list.begin(); i != speaker_list.end(); ++i)
	{
		LLPointer<LLSpeaker> s = *i;
		if (s->mSpeechVolume > 0 || s->mStatus == LLSpeaker::STATUS_SPEAKING)
		{
			id = s->mID;
			break;
		}
	}

	if (!id.isNull())
	{
		mOutputMonitor->setVisible(TRUE);
		mOutputMonitor->setSpeakerId(id);
	}
	else
	{
		mOutputMonitor->setVisible(FALSE);
	}
}

void LLNearbyChatBar::sendChatFromViewer(const std::string &utf8text, EChatType type, BOOL animate)
{
	sendChatFromViewer(utf8str_to_wstring(utf8text), type, animate);
}

void LLNearbyChatBar::sendChatFromViewer(const LLWString &wtext, EChatType type, BOOL animate)
{
	// Look for "/20 foo" channel chats.
	S32 channel = 0;
	LLWString out_text = stripChannelNumber(wtext, &channel);
	std::string utf8_out_text = wstring_to_utf8str(out_text);
	std::string utf8_text = wstring_to_utf8str(wtext);

	utf8_text = utf8str_trim(utf8_text);
	if (!utf8_text.empty())
	{
		utf8_text = utf8str_truncate(utf8_text, MAX_STRING - 1);
	}

// [RLVa:KB] - Checked: 2010-03-27 (RLVa-1.2.0b) | Modified: RLVa-1.2.0b
	if ( (0 == channel) && (rlv_handler_t::isEnabled()) )
	{
		// Adjust the (public) chat "volume" on chat and gestures (also takes care of playing the proper animation)
		if ( ((CHAT_TYPE_SHOUT == type) || (CHAT_TYPE_NORMAL == type)) && (gRlvHandler.hasBehaviour(RLV_BHVR_CHATNORMAL)) )
			type = CHAT_TYPE_WHISPER;
		else if ( (CHAT_TYPE_SHOUT == type) && (gRlvHandler.hasBehaviour(RLV_BHVR_CHATSHOUT)) )
			type = CHAT_TYPE_NORMAL;
		else if ( (CHAT_TYPE_WHISPER == type) && (gRlvHandler.hasBehaviour(RLV_BHVR_CHATWHISPER)) )
			type = CHAT_TYPE_NORMAL;

		animate &= !gRlvHandler.hasBehaviour( (!RlvUtil::isEmote(utf8_text)) ? RLV_BHVR_REDIRCHAT : RLV_BHVR_REDIREMOTE );
	}
// [/RLVa:KB]

	// Don't animate for chats people can't hear (chat to scripts)
	if (animate && (channel == 0))
	{
		if (type == CHAT_TYPE_WHISPER)
		{
			lldebugs << "You whisper " << utf8_text << llendl;
			gAgent.sendAnimationRequest(ANIM_AGENT_WHISPER, ANIM_REQUEST_START);
		}
		else if (type == CHAT_TYPE_NORMAL)
		{
			lldebugs << "You say " << utf8_text << llendl;
			gAgent.sendAnimationRequest(ANIM_AGENT_TALK, ANIM_REQUEST_START);
		}
		else if (type == CHAT_TYPE_SHOUT)
		{
			lldebugs << "You shout " << utf8_text << llendl;
			gAgent.sendAnimationRequest(ANIM_AGENT_SHOUT, ANIM_REQUEST_START);
		}
		else
		{
			llinfos << "send_chat_from_viewer() - invalid volume" << llendl;
			return;
		}
	}
	else
	{
		if (type != CHAT_TYPE_START && type != CHAT_TYPE_STOP)
		{
			lldebugs << "Channel chat: " << utf8_text << llendl;
		}
	}

	send_chat_from_viewer(utf8_out_text, type, channel);
}

// static 
void LLNearbyChatBar::startChat(const char* line)
{
	LLBottomTray *bt = LLBottomTray::getInstance();

	if (!bt)
		return;

	LLNearbyChatBar* cb = bt->getNearbyChatBar();

	if (!cb )
		return;

	bt->setVisible(TRUE);
	cb->mChatBox->setFocus(TRUE);

	if (line)
	{
		std::string line_string(line);
		cb->mChatBox->setText(line_string);
	}

	cb->mChatBox->setCursorToEnd();
}

// Exit "chat mode" and do the appropriate focus changes
// static
void LLNearbyChatBar::stopChat()
{
	LLBottomTray *bt = LLBottomTray::getInstance();

	if (!bt)
		return;

	LLNearbyChatBar* cb = bt->getNearbyChatBar();

	if (!cb)
		return;

	cb->mChatBox->setFocus(FALSE);

 	// stop typing animation
 	gAgent.stopTyping();
}

// If input of the form "/20foo" or "/20 foo", returns "foo" and channel 20.
// Otherwise returns input and channel 0.
LLWString LLNearbyChatBar::stripChannelNumber(const LLWString &mesg, S32* channel)
{
	if (mesg[0] == '/'
		&& mesg[1] == '/')
	{
		// This is a "repeat channel send"
		*channel = sLastSpecialChatChannel;
		return mesg.substr(2, mesg.length() - 2);
	}
	else if (mesg[0] == '/'
			 && mesg[1]
			 && ( LLStringOps::isDigit(mesg[1])
				 || (mesg[1] == '-'
				 	&& LLStringOps::isDigit(mesg[2]))))
	{
		// This a special "/20" speak on a channel
		S32 pos = 0;
		if(mesg[1] == '-')
			pos++;
		// Copy the channel number into a string
		LLWString channel_string;
		llwchar c;
		do
		{
			c = mesg[pos+1];
			channel_string.push_back(c);
			pos++;
		}
		while(c && pos < 64 && LLStringOps::isDigit(c));
		
		// Move the pointer forward to the first non-whitespace char
		// Check isspace before looping, so we can handle "/33foo"
		// as well as "/33 foo"
		while(c && iswspace(c))
		{
			c = mesg[pos+1];
			pos++;
		}
		
		sLastSpecialChatChannel = strtol(wstring_to_utf8str(channel_string).c_str(), NULL, 10);
		if(mesg[1] == '-')
			sLastSpecialChatChannel = -sLastSpecialChatChannel;
		*channel = sLastSpecialChatChannel;
		return mesg.substr(pos, mesg.length() - pos);
	}
	else
	{
		// This is normal chat.
		*channel = 0;
		return mesg;
	}
}

//void send_chat_from_viewer(const std::string& utf8_out_text, EChatType type, S32 channel)
// [RLVa:KB] - Checked: 2010-02-27 (RLVa-1.2.0b) | Modified: RLVa-0.2.2a
void send_chat_from_viewer(std::string utf8_out_text, EChatType type, S32 channel)
// [/RLVa:KB]
{
// [RLVa:KB] - Checked: 2010-02-27 (RLVa-1.2.0b) | Modified: RLVa-1.2.0a
	// Only process chat messages (ie not CHAT_TYPE_START, CHAT_TYPE_STOP, etc)
	if ( (rlv_handler_t::isEnabled()) && ( (CHAT_TYPE_WHISPER == type) || (CHAT_TYPE_NORMAL == type) || (CHAT_TYPE_SHOUT == type) ) )
	{
		if (0 == channel)
		{
			// (We already did this before, but LLChatHandler::handle() calls this directly)
			if ( ((CHAT_TYPE_SHOUT == type) || (CHAT_TYPE_NORMAL == type)) && (gRlvHandler.hasBehaviour(RLV_BHVR_CHATNORMAL)) )
				type = CHAT_TYPE_WHISPER;
			else if ( (CHAT_TYPE_SHOUT == type) && (gRlvHandler.hasBehaviour(RLV_BHVR_CHATSHOUT)) )
				type = CHAT_TYPE_NORMAL;
			else if ( (CHAT_TYPE_WHISPER == type) && (gRlvHandler.hasBehaviour(RLV_BHVR_CHATWHISPER)) )
				type = CHAT_TYPE_NORMAL;

			// Redirect chat if needed
			if ( ( (gRlvHandler.hasBehaviour(RLV_BHVR_REDIRCHAT) || (gRlvHandler.hasBehaviour(RLV_BHVR_REDIREMOTE)) ) && 
				 (gRlvHandler.redirectChatOrEmote(utf8_out_text)) ) )
			{
				return;
			}

			// Filter public chat if sendchat restricted
			if (gRlvHandler.hasBehaviour(RLV_BHVR_SENDCHAT))
				gRlvHandler.filterChat(utf8_out_text, true);
		}
		else
		{
			// Don't allow chat on a non-public channel if sendchannel restricted (unless the channel is an exception)
			if ( (gRlvHandler.hasBehaviour(RLV_BHVR_SENDCHANNEL)) && (!gRlvHandler.isException(RLV_BHVR_SENDCHANNEL, channel)) )
				return;

			// Don't allow chat on debug channel if @sendchat, @redirchat or @rediremote restricted (shows as public chat on viewers)
			if (CHAT_CHANNEL_DEBUG == channel)
			{
				bool fIsEmote = RlvUtil::isEmote(utf8_out_text);
				if ( (gRlvHandler.hasBehaviour(RLV_BHVR_SENDCHAT)) || 
					 ((!fIsEmote) && (gRlvHandler.hasBehaviour(RLV_BHVR_REDIRCHAT))) || 
					 ((fIsEmote) && (gRlvHandler.hasBehaviour(RLV_BHVR_REDIREMOTE))) )
				{
					return;
				}
			}
		}
	}
// [/RLVa:KB]

	LLMessageSystem* msg = gMessageSystem;
	if(channel >= 0)
	{
		msg->newMessageFast(_PREHASH_ChatFromViewer);
		msg->nextBlockFast(_PREHASH_AgentData);
		msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
		msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
		msg->nextBlockFast(_PREHASH_ChatData);
		msg->addStringFast(_PREHASH_Message, utf8_out_text);
		msg->addU8Fast(_PREHASH_Type, type);
		msg->addS32("Channel", channel);
	}
	else
	{
		msg->newMessage("ScriptDialogReply");
		msg->nextBlock("AgentData");
		msg->addUUID("AgentID", gAgent.getID());
		msg->addUUID("SessionID", gAgent.getSessionID());
		msg->nextBlock("Data");
		msg->addUUID("ObjectID", gAgent.getID());
		msg->addS32("ChatChannel", channel);
		msg->addS32("ButtonIndex", 0);
		msg->addString("ButtonLabel", utf8_out_text);
	}

	gAgent.sendReliableMessage();

	LLViewerStats::getInstance()->incStat(LLViewerStats::ST_CHAT_COUNT);
}

class LLChatHandler : public LLCommandHandler
{
public:
	// not allowed from outside the app
	LLChatHandler() : LLCommandHandler("chat", UNTRUSTED_BLOCK) { }

    // Your code here
	bool handle(const LLSD& tokens, const LLSD& query_map,
				LLMediaCtrl* web)
	{
		bool retval = false;
		// Need at least 2 tokens to have a valid message.
		if (tokens.size() < 2)
		{
			retval = false;
		}
		else
		{
		S32 channel = tokens[0].asInteger();
			// VWR-19499 Restrict function to chat channels greater than 0.
			if ((channel > 0) && (channel < 2147483647))
			{
				retval = true;
		// Send unescaped message, see EXT-6353.
		std::string unescaped_mesg (LLURI::unescape(tokens[1].asString()));
		send_chat_from_viewer(unescaped_mesg, CHAT_TYPE_NORMAL, channel);
			}
			else
			{
				retval = false;
				// Tell us this is an unsupported SLurl.
			}
		}
		return retval;
	}
};

// Creating the object registers with the dispatcher.
LLChatHandler gChatHandler;


