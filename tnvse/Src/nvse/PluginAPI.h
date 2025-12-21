#pragma once
#include <string>
#include <vector>

#include "ITypes.h"

struct CommandInfo;
struct ParamInfo;
class TESObjectREFR;
class Script;
class TESForm;
struct ScriptEventList;
struct ArrayKey;

namespace PluginAPI {
    class ArrayAPI;
}

struct PluginInfo;

typedef UInt32 PluginHandle; // treat this as an opaque type

enum : uint32_t {
    kPluginHandle_Invalid = 0xFFFFFFFF,
};

enum {
    kInterface_Serialization = 0,
    kInterface_Console,

    // Added v0002
    kInterface_Messaging,
    kInterface_CommandTable,

    // Added v0004
    kInterface_StringVar,
    kInterface_ArrayVar,
    kInterface_Script,

    // Added v0005 - version bumped to 3
    kInterface_Data,
    // Added v0006
    kInterface_EventManager,
    kInterface_Logging,

    kInterface_Max
};

struct ExpressionEvaluatorUtils;

struct NVSEInterface {
    UInt32 nvseVersion;
    UInt32 runtimeVersion;
    UInt32 editorVersion;
    UInt32 isEditor;
    bool (*RegisterCommand)(CommandInfo* info);
    void (*SetOpcodeBase)(UInt32 opcode);
    void* (*QueryInterface)(UInt32 id);
    PluginHandle (*GetPluginHandle)(void);
};

struct NVSEConsoleInterface {
    enum {
        kVersion = 3
    };

    UInt32 version;
    bool (*RunScriptLine)(const char* buf, TESObjectREFR* object);
    bool (*RunScriptLine2)(const char* buf, TESObjectREFR* callingRefr, bool bSuppressConsoleOutput);
};

struct NVSEMessagingInterface {
    struct Message {
        const char* sender;
        UInt32 type;
        UInt32 dataLen;
        void* data;
    };

    typedef void (*EventCallback)(Message* msg);

    enum {
        kVersion = 4
    };

    // NVSE messages
    enum {
        kMessage_PostLoad, // sent to registered plugins once all plugins have been loaded (no data)
        kMessage_ExitGame, // exit to windows from main menu or in-game menu
        kMessage_ExitToMainMenu, // exit to main menu from in-game menu
        kMessage_LoadGame,
        // Dispatched immediately before plugin serialization callbacks invoked, after savegame has been read by Fallout
        // dataLen: length of file path, data: char* file path of .fos savegame file
        // Receipt of this message does not *guarantee* the serialization callback will be invoked
        // as there may be no .nvse file associated with the savegame
        kMessage_SaveGame, // as above
        kMessage_ScriptPrecompile, // EDITOR+RUNTIME: Dispatched when a script is about to be compiled.
        // To custom-compile the script yourself during this step, set script->info.compiled to true.
        // Alternatively, scriptBuffer->errorCode can be set to 1 to prevent the script from compiling entirely.
        // If custom-compiling, certain Script* variables should be set, and SetEditorID should be called if there was a scriptname extracted.
        // data: ScriptAndScriptBuffer* to the script + scriptBuffer representing the script under compilation
        // dataLen: sizeof(ScriptAndScriptBuffer)
        kMessage_PreLoadGame, // dispatched immediately before savegame is read by Fallout
        // dataLen: length of file path, data: char* file path of .fos savegame file
        kMessage_ExitGame_Console, // exit game using 'qqq' console command
        kMessage_PostLoadGame,
        //dispatched after an attempt to load a saved game has finished (the game's LoadGame() routine
        //has returned). You will probably want to handle this event if your plugin uses a Preload callback
        //as there is a chance that after that callback is invoked the game will encounter an error
        //while loading the saved game (eg. corrupted save) which may require you to reset some of your
        //plugin state.
        //data: bool, true if game successfully loaded, false otherwise */
        kMessage_PostPostLoad,
        // sent right after kMessage_PostLoad to facilitate the correct dispatching/registering of messages/listeners
        // plugins may register as listeners during the first callback while deferring dispatches until the next
        kMessage_RuntimeScriptError, // dispatched when an NVSE script error is encountered during runtime/
        // data: char* errorMessageText
        // added for kVersion = 2
        kMessage_DeleteGame, // sent right before deleting the .nvse cosave and the .fos save.
        // dataLen: length of file path, data: char* file path of .fos savegame file
        kMessage_RenameGame, // sent right before renaming the .nvse cosave and the .fos save.
        // dataLen: length of old file path, data: char* old file path of .fos savegame file
        // you are expected to save the data and wait for kMessage_RenameNewGame
        kMessage_RenameNewGame, // sent right after kMessage_RenameGame.
        // dataLen: length of new file path, data: char* new file path of .fos savegame file
        kMessage_NewGame, // sent right before iterating through plugins newGame.
        // dataLen: 0, data: NULL
        // added for kVersion == 3
        kMessage_DeleteGameName, // version of the messages sent with a save file name instead of a save file path.
        kMessage_RenameGameName,
        kMessage_RenameNewGameName,
        // added for kVersion == 4 (xNVSE)
        kMessage_DeferredInit,
        kMessage_ClearScriptDataCache,
        kMessage_MainGameLoop, // called each game loop
        kMessage_ScriptCompile, // EDITOR: called after successful script compilation in GECK. data: pointer to Script
        // RUNTIME: also gets called after successful script compilation at runtime via functions.
        kMessage_EventListDestroyed,
        // called before a script event list is destroyed, dataLen: 4, data: ScriptEventList* ptr
        kMessage_PostQueryPlugins, // called after all plugins have been queried
    };

    UInt32 version;
    bool (*RegisterListener)(PluginHandle listener, const char* sender, EventCallback handler);
    bool (*Dispatch)(PluginHandle sender, UInt32 messageType, void* data, UInt32 dataLen, const char* receiver);
};

struct PluginInfo {
    enum {
        kInfoVersion = 1
    };

    UInt32 infoVersion;
    const char* name;
    UInt32 version;
};

typedef bool (*_NVSEPlugin_Query)(const NVSEInterface* nvse, PluginInfo* info);
typedef bool (*_NVSEPlugin_Load)(const NVSEInterface* nvse);

struct NVSEStringVarInterface
{
	enum {
		kVersion = 1
	};

	UInt32		version;

	// The returned C-string is valid for as long as the string_var indicated by stringID remains unchanged.
	// For example, the returned string pointer may become invalid if the string_var has to re-allocate when being set to another string value.
	// To be safe, copy the returned C-string if you intend to keep it for some time.
	const char* (*GetString)(UInt32 stringID);

	// Passed C-string will be copied, so it is safe to delete it afterwards.
	void		(*SetString)(UInt32 stringID, const char* newValue);

	// Passed C-string will be copied, so it is safe to delete it afterwards.
	UInt32(*CreateString)(const char* value, void* owningScript);

	// (is RegisterStringVarInterface() in GameAPI.h)
	void		(*Register)(NVSEStringVarInterface* intfc);
};


struct NVSESerializationInterface
{
    enum
    {
        kVersion = 2,
    };

    typedef void (*EventCallback)(void* reserved);

    UInt32	version;
    void	(*SetSaveCallback)(PluginHandle plugin, EventCallback callback);
    void	(*SetLoadCallback)(PluginHandle plugin, EventCallback callback);
    void	(*SetNewGameCallback)(PluginHandle plugin, EventCallback callback);

    bool	(*WriteRecord)(UInt32 type, UInt32 version, const void* buf, UInt32 length);
    bool	(*OpenRecord)(UInt32 type, UInt32 version);
    bool	(*WriteRecordData)(const void* buf, UInt32 length);

    bool	(*GetNextRecordInfo)(UInt32* type, UInt32* version, UInt32* length);
    UInt32(*ReadRecordData)(void* buf, UInt32 length);

    // take a refid as stored in the loaded save file and resolve it using the currently
    // loaded list of mods. All refids stored in a save file must be run through this
    // function to account for changing mod lists. This returns true on success, and false
    // if the mod owning the RefID was unloaded.
    bool	(*ResolveRefID)(UInt32 refID, UInt32* outRefID);

    void	(*SetPreLoadCallback)(PluginHandle plugin, EventCallback callback);

    // returns a full path to the last loaded save game
    const char* (*GetSavePath)();

    // Peeks at the data without interfiring with the current position
    UInt32(*PeekRecordData)(void* buf, UInt32 length);

    void	(*WriteRecord8)(UInt8 inData);
    void	(*WriteRecord16)(UInt16 inData);
    void	(*WriteRecord32)(UInt32 inData);
    void	(*WriteRecord64)(const void* inData);

    UInt8(*ReadRecord8)();
    UInt16(*ReadRecord16)();
    UInt32(*ReadRecord32)();
    void	(*ReadRecord64)(void* outData);

    void	(*SkipNBytes)(UInt32 byteNum);
};

struct NVSEArrayVarInterface
{
	enum {
		kVersion = 2
	};

	struct Array;

	struct Element
	{
	protected:
		union
		{
			char* str;
			Array* arr;
			TESForm* form;
			double		num;
		};
		UInt8		type;

		friend class PluginAPI::ArrayAPI;
		friend class ArrayVar;
	public:
		enum
		{
			kType_Invalid,
			kType_Numeric,
			kType_Form,
			kType_String,
			kType_Array,
		};
	};

	Array* (*CreateArray)(const Element* data, UInt32 size, Script* callingScript);
	Array* (*CreateStringMap)(const char** keys, const NVSEArrayVarInterface::Element* values, UInt32 size, Script* callingScript);
	Array* (*CreateMap)(const double* keys, const NVSEArrayVarInterface::Element* values, UInt32 size, Script* callingScript);

	bool	(*AssignCommandResult)(Array* arr, double* dest);
	void	(*SetElement)(Array* arr, const Element& key, const Element& value);
	void	(*AppendElement)(Array* arr, const Element& value);

	UInt32(*GetArraySize)(Array* arr);
	Array* (*LookupArrayByID)(UInt32 id);
	bool	(*GetElement)(Array* arr, const Element& key, Element& outElement);
	bool	(*GetElements)(Array* arr, Element* elements, Element* keys);

	// version 2
	UInt32(*GetArrayPacked)(Array* arr);

	enum ContainerTypes
	{
		kArrType_Invalid = -1,
		kArrType_Array = 0,
		kArrType_Map,
		kArrType_StringMap
	};

	int		(*GetContainerType)(Array* arr);
	bool	(*ArrayHasKey)(Array* arr, const Element& key);
};

struct NVSEEventManagerInterface
{
	typedef void (*NativeEventHandler)(TESObjectREFR* thisObj, void* parameters);

	// Mostly used for filtering information.
	enum ParamType : UInt8
	{
		eParamType_Float = 0,
		eParamType_Int,
		eParamType_String,
		eParamType_Array,

		// All the form-type ParamTypes support formlist filters, which will check if the dispatched form matches with any of the forms in the list.
		// In case a reference is dispatched, it can be filtered by a baseForm.
		eParamType_RefVar,
		eParamType_AnyForm = eParamType_RefVar,

		// Behaves normally if a reference filter is set up for a param of Reference Type.
		// Otherwise, if a regular baseform is the filter, will match the dispatched reference arg's baseform to the filter.
		// Else, if the filter is a formlist, will do the above but for each element in the list.
		eParamType_Reference,

		// When attempting to set an event handler, if the filter-to-set is a reference the paramType is BaseForm, will reject that filter.
		// Otherwise, behaves the same as eParamType_RefVar.
		eParamType_BaseForm,

		eParamType_Invalid,
		eParamType_Anything,

		// The Ptr-type params signify a pointer to a value will be passed, which will be dereferenced between each call.
		// Otherwise, they work like their regular counterparts.
		// This is useful if a param needs to have its value updated in-between calls to event handlers.
		// NOTE: if passing a ptr to a thread-safe dispatch function, it MUST point to a statically-defined value, to avoid using an invalid pointer if the dispatch was delayed.
		eParamType_FloatPtr,
		eParamType_IntPtr,
		eParamType_StringPtr,
		eParamType_ArrayPtr,
		eParamType_RefVarPtr,
		eParamType_AnyFormPtr = eParamType_RefVarPtr,
		eParamType_ReferencePtr,
		eParamType_BaseFormPtr
	};
	static [[nodiscard]] bool IsFormParam(ParamType pType)
	{
		return pType == eParamType_RefVar || pType == eParamType_Reference || pType == eParamType_BaseForm
			|| pType == eParamType_Anything;
	}
	static [[nodiscard]] bool IsPtrParam(ParamType pType)
	{
		return (pType >= eParamType_FloatPtr) && (pType <= eParamType_BaseFormPtr);
	}
	// Gets the regular non-ptr version of the param
	static [[nodiscard]] ParamType GetNonPtrParamType(ParamType pType)
	{
		if (IsPtrParam(pType))
		{
			return static_cast<ParamType>(pType - 9);
		}
		return pType;
	}

	enum EventFlags : UInt32
	{
		kFlags_None = 0,

		// If on, will remove all set handlers for the event every game load.
		kFlag_FlushOnLoad = 1 << 0,

		// Events with this flag do not need to provide ParamTypes when defined.
		// However, arg types must still be known when dispatching the event.
		// For scripts, DispatchEventAlt will provide the args + their types.
		// For plugins, no method is exposed to dispatch such an event,
		// since it is recommended to define the event with ParamTypes instead.
		kFlag_HasUnknownArgTypes = 1 << 1,

		// Allows scripts to dispatch the event.
		// This comes at the risk of not knowing if some other scripted mod is dispatching your event.
		kFlag_AllowScriptDispatch = 1 << 2,

		// When implicitly creating a new event via the script function SetEventHandler(Alt), these flags are set.
		kFlag_IsUserDefined = kFlag_HasUnknownArgTypes | kFlag_AllowScriptDispatch,

		// If on, for events with a DispatchCallback, will report an error if no callback result is passed by an event handler.
		// (This happens if SetFunctionValue isn't used anywhere during a script callback, for example).
		// (Reporting an error will also prevent other handlers from executing).
		// Otherwise, the callback will be sent an ArrayElement with Invalid type. 
		kFlag_ReportErrorIfNoResultGiven = 1 << 3
	};

	// Registers a new event which can be dispatched to scripts and plugins.
	// Returns false if event with name already exists.
	bool (*RegisterEvent)(const char* name, UInt8 numParams, ParamType* paramTypes, EventFlags flags);

	// Dispatch an event that has been registered with RegisterEvent.
	// Variadic arguments are passed as parameters to script / function.
	// Returns false if an error occurred.
	bool (*DispatchEvent)(const char* eventName, TESObjectREFR* thisObj, ...);

	enum DispatchReturn : int8_t
	{
		kRetn_UnknownEvent = -2,  // for plugins, events are supposed to be pre-defined.
		kRetn_GenericError = -1,  // anything > -1 is a good result.
		kRetn_Normal = 0,
		kRetn_EarlyBreak,
		kRetn_Deferred,	//for the "ThreadSafe" DispatchEvent functions.
	};
	typedef bool (*DispatchCallback)(NVSEArrayVarInterface::Element& result, void* anyData);

	// If resultCallback is not null, then it is called for each SCRIPT event handler that is dispatched, which allows checking the result of each dispatch.
	// If the callback returns false, then dispatching for the event will end prematurely, and this returns kRetn_EarlyBreak.
	// 'anyData' arg is passed to the callbacks.
	DispatchReturn(*DispatchEventAlt)(const char* eventName, DispatchCallback resultCallback, void* anyData, TESObjectREFR* thisObj, ...);

	// Special priorities used for the event priority system.
	// Greatest priority = will run first, lowest = will run last.
	enum SpecialHandlerPriorities : int
	{
		// Used as a special case when searching through handlers; invalid priority = unfiltered for priority.
		// A handler CANNOT be set with this priority.
		// However, negative priorities ARE allowed to be set.
		// When removing handlers, this value can be used to remove handlers regardless of priority.
		kHandlerPriority_Invalid = 0,

		// When setting a handler, used if no priority is specified.
		kHandlerPriority_Default = 1,

		kHandlerPriority_Max = 9999,
		kHandlerPriority_Min = -9999
	};

	// Similar to script function SetEventHandler, allows you to set a native function that gets called back on events
	// Unlike SetEventHandler, the event must already be defined before this function is called.
	// Default priority (1) is given for the handler.
	bool (*SetNativeEventHandler)(const char* eventName, NativeEventHandler func);

	// Same as script function RemoveEventHandler but for native functions
	// Invalid priority (0) is implicitly passed, so that all handlers for the event, regardless of priority, will be removed.
	bool (*RemoveNativeEventHandler)(const char* eventName, NativeEventHandler func);

	bool (*RegisterEventWithAlias)(const char* name, const char* alias, UInt8 numParams, ParamType* paramTypes, EventFlags flags);

	// If passed as non-null, will be called after all handlers have been dispatched.
	// The "ThreadSafe" dispatch functions can delay the dispatch by a frame, hence why this callback is needed.
	// Useful to potentially clear heap-allocated memory for the dispatch.
	typedef void (*PostDispatchCallback)(void* anyData, DispatchReturn retn);

	// Same as DispatchEvent, but if attempting to dispatch outside of the game's main thread, the dispatch will be deferred.
	// WARNING: must ensure data will not be invalid if the dispatch is deferred.
	// Recommended to avoid potential multithreaded crashes, usually related to Console_Print.
	bool (*DispatchEventThreadSafe)(const char* eventName, PostDispatchCallback postCallback, TESObjectREFR* thisObj, ...);

	// Same as DispatchEventAlt, but if attempting to dispatch outside of the game's main thread, the dispatch will be deferred.
	// WARNING: must ensure data will not be invalid if the dispatch is deferred.
	// Recommended to avoid potential multithreaded crashes, usually related to Console_Print.
	DispatchReturn(*DispatchEventAltThreadSafe)(const char* eventName, DispatchCallback resultCallback, void* anyData,
		PostDispatchCallback postCallback, TESObjectREFR* thisObj, ...);

	// Like the script function SetFunctionValue, but for native handlers.
	// If never called, then a nullptr element is passed by default.
	// WARNING: must ensure the pointer remains valid AFTER the native EventHandler function is executed.
	// The pointer can be invalidated during or after a DispatchCallback.
	void (*SetNativeHandlerFunctionValue)(NVSEArrayVarInterface::Element& value);

	// 'pluginHandle' and 'handlerName' provide easier debugging, i.e. when dumping handlers.
	// Returns false if providing an invalid PluginHandle (can pass null handlerName, but not recommended).
	bool (*SetNativeEventHandlerWithPriority)(const char* eventName, NativeEventHandler func,
		PluginHandle pluginHandle, const char* handlerName, int priority);

	bool (*RemoveNativeEventHandlerWithPriority)(const char* eventName, NativeEventHandler func, int priority);

	// A quick way to check for a handler priority conflict, i.e. if a handler is expected to run first. 
	/* If any non-excluded handlers are found above or at 'priority', returns false.
	 * 'startPriority' is assumed to NOT be 0 (which is an invalid priority).
	 * 	Returns false if 'func' is not found at 'startPriority'. It can appear elsewhere and will be ignored.
	*/
	// All '...ToIgnore' parameters are optional, i.e they can be nullptr and the 'num...' args can be set to 0.
	// 'scriptsToIgnore' can be nullptr, a Script*, or a formlist.
	bool (*IsEventHandlerFirst)(const char* eventName, NativeEventHandler func, int startPriority,
		TESForm** scriptsToIgnore, UInt32 numScriptsToIgnore,
		const char** pluginsToIgnore, UInt32 numPluginsToIgnore,
		const char** pluginHandlersToIgnore, UInt32 numPluginHandlersToIgnore);

	bool (*IsEventHandlerLast)(const char* eventName, NativeEventHandler func, int startPriority,
		TESForm** scriptsToIgnore, UInt32 numScriptsToIgnore,
		const char** pluginsToIgnore, UInt32 numPluginsToIgnore,
		const char** pluginHandlersToIgnore, UInt32 numPluginHandlersToIgnore);

	// Returns a Map-type array with all the priority-conflicting event handlers, i.e the events that will run before the 'func' handler. 
	// Each key in the map is a priority, and each value is a 2-element array containing:
	//		[0] = the handler, [1] = a stringmap of filters.
	/* 'priority' is assumed to NOT be 0 (which is an invalid priority).
	 * The array includes handlers that are at 'priority', as they can potentially lead to conflicts.
	*/
	// All '...ToIgnore' parameters are optional, i.e they can be nullptr and the 'num...' args can be set to 0.
	// Returns nullptr if 'func' is not found at 'priority'. It can appear elsewhere and will be ignored.
	NVSEArrayVarInterface::Array* (*GetHigherPriorityEventHandlers)(const char* eventName, NativeEventHandler func, int priority,
		TESForm** scriptsToIgnore, UInt32 numScriptsToIgnore,
		const char** pluginsToIgnore, UInt32 numPluginsToIgnore,
		const char** pluginHandlersToIgnore, UInt32 numPluginHandlersToIgnore);

	NVSEArrayVarInterface::Array* (*GetLowerPriorityEventHandlers)(const char* eventName, NativeEventHandler func, int priority,
		TESForm** scriptsToIgnore, UInt32 numScriptsToIgnore,
		const char** pluginsToIgnore, UInt32 numPluginsToIgnore,
		const char** pluginHandlersToIgnore, UInt32 numPluginHandlersToIgnore);
};

enum CommandReturnType : UInt8
{
	kRetnType_Default,
	kRetnType_Form,
	kRetnType_String,
	kRetnType_Array,
	kRetnType_ArrayIndex,
	kRetnType_Ambiguous,

	kRetnType_Max
};

const char* CommandReturnTypeToString(CommandReturnType in);

class PluginManager
{
public:

	PluginInfo* GetInfoByName(const char* name);
	PluginInfo* GetInfoFromHandle(PluginHandle handle);
	PluginInfo* GetInfoFromBase(UInt32 baseOpcode);
	PluginInfo* GetInfoByDLLName(const char* DLLName);
	const char* GetPluginNameFromHandle(PluginHandle handle);

	UInt32			GetNumPlugins(void);
	UInt32			GetBaseOpcode(UInt32 idx);
	PluginHandle	LookupHandleFromBaseOpcode(UInt32 baseOpcode);

private:
	struct LoadedPlugin
	{
		HMODULE		handle;
		PluginInfo	info;
		UInt32		baseOpcode;

		_NVSEPlugin_Query	query;
		_NVSEPlugin_Load	load;

		char path[MAX_PATH]{};			// Added version 4.5 Beta 7
	};

	struct PluginLoadState
	{
		LoadedPlugin plugin{};
		std::string loadStatus;
		bool querySuccess = false;
		bool loadSuccess = false;
	};

	typedef std::vector <LoadedPlugin>	LoadedPluginList;

	std::string			m_pluginDirectory;
	LoadedPluginList	m_plugins;

	static LoadedPlugin* s_currentLoadingPlugin;
	static PluginHandle		   s_currentPluginHandle;
};

extern PluginManager	g_pluginManager;

extern CommandInfo kCommandInfo_IsPluginInstalled;
extern CommandInfo kCommandInfo_GetPluginVersion;

typedef UInt32(__stdcall* _GetLNEventMask)(const char* eventName);
extern _GetLNEventMask GetLNEventMask;
typedef bool(__stdcall* _ProcessLNEventHandler)(UInt32 eventMask, Script* udfScript, bool addEvt, TESForm* formFilter, UInt32 numFilter);
extern _ProcessLNEventHandler ProcessLNEventHandler;

struct NVSECommandTableInterface
{
	enum {
		kVersion = 2
	};

	UInt32	version;
	const PluginInfo* GetPluginInfoByName(const char* pluginName) { return g_pluginManager.GetInfoByName(pluginName); }	// Returns a pointer to the PluginInfo of the NVSE plugin of the specified name; returns NULL is the plugin is not loaded.
	const PluginInfo* GetPluginInfoByDLLName(const char* dllName) { return g_pluginManager.GetInfoByDLLName(dllName); }	// Returns a pointer to the PluginInfo of the NVSE plugin with the specified DLL name; returns NULL if the plugin is not loaded.
};