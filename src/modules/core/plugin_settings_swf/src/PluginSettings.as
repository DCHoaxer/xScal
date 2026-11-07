package
{
   import Shared.AS3.MenuComponent;
   import com.brokenfunction.json.decodeJson;
   import flash.display.DisplayObject;
   import flash.display.Sprite;
   import flash.events.Event;
   import flash.events.FocusEvent;
   import flash.utils.getDefinitionByName;
   import flash.utils.getTimer;

   // Document class of PluginSettings.swf, loaded into seventysixmenu.swf by xScal's core module. Adds the
   // main menu entry and opens the window as a component of the game's menu stack.
   public class PluginSettings extends Sprite
   {

      private static const MAIN_LIST_CHECK_FRAMES:int = 10;

      // An Esc press also reaches the game as Back within this time; that Back must not close another layer.
      private static const ESCAPE_SAME_PRESS_MS:int = 700;

      private static const CLOSE_DELAY_FRAMES:int = 2;

      private static const CLOSE_WAIT_MAX_MS:int = 1500;

      // The core module attaches xScal's call() to this object.
      public var __SFCodeObj:Object = {};

      private var window:PluginSettingsWindow;

      // The active component while the game's Back runs: Back activates the previous component in its
      // history, and this one keeps the main list out of that step.
      private var backstop:MenuComponent;

      private var mainMenu:MainMenuLink;

      private var root1:* = null;

      private var frameCount:int = 0;

      private var published:Boolean = false;

      private var windowOpen:Boolean = false;

      private var activatingBackstop:Boolean = false;

      private var backPending:Boolean = false;

      private var closePending:Boolean = false;

      private var closeRequestedAt:int = 0;

      private var closeDelayFrames:int = 0;

      private var lastEscapeAt:int = -100000;

      private var backHint:* = null;

      public function PluginSettings()
      {
         super();
         this.backstop = new MenuComponent();
         this.backstop.visible = false;
         this.backstop.addEventListener(FocusEvent.FOCUS_IN,this.onBackstopFocused);
         this.addChild(this.backstop);
         this.window = new PluginSettingsWindow();
         this.window.readPlugins = this.readPlugins;
         this.window.writeOption = this.writeOption;
         this.window.onEscape = this.onEscapePressed;
         this.addChild(this.window);
         this.mainMenu = new MainMenuLink(this.openWindow);
         this.mainMenu.setEntryText(Texts.get("entry"));
         this.addEventListener(Event.ENTER_FRAME,this.onEnterFrame);
      }

      // Read by the core module every frame: mainmenu.swf draws the news panel under this window.
      public function get isWindowOpen() : Boolean
      {
         return this.windowOpen;
      }

      // Set by the core module once the SWF has loaded.
      public function set language(code:String) : void
      {
         Texts.language = code;
         this.mainMenu.setEntryText(Texts.get("entry"));
      }

      private function readPlugins() : Array
      {
         try
         {
            var decoded:* = decodeJson.decode(String(this.__SFCodeObj.call("xsGetPluginOptions",2)),false);
            return decoded != null && decoded.plugins is Array ? decoded.plugins as Array : [];
         }
         catch(failure:Error)
         {
         }
         return [];
      }

      private function writeOption(pluginId:String, optionId:String, value:*) : Boolean
      {
         try
         {
            return this.__SFCodeObj.call("xsSetPluginOption",pluginId,optionId,value) === true;
         }
         catch(failure:Error)
         {
         }
         return false;
      }

      private function onEnterFrame(event:Event) : void
      {
         this.frameCount++;
         if(this.frameCount % MAIN_LIST_CHECK_FRAMES == 0)
         {
            if(this.root1 == null || this.root1.stage == null)
            {
               this.root1 = this.findMenuRoot();
            }
            if(this.root1 != null)
            {
               this.publishCodeOwner();
               this.mainMenu.update(this.root1);
            }
         }
         if(this.closePending)
         {
            this.advanceClose();
         }
         else if(this.windowOpen && (this.backPending || !this.window.visible || !this.window.Active))
         {
            this.backPending = false;
            this.onBackedOut();
         }
      }

      private function findMenuRoot() : *
      {
         var node:DisplayObject = this;
         while(node != null)
         {
            if(isMenuRoot(node))
            {
               return node;
            }
            node = node.parent;
         }
         return null;
      }

      private static function isMenuRoot(node:*) : Boolean
      {
         try
         {
            return node.MainList != null && node.changeActiveComponent is Function;
         }
         catch(readFailure:Error)
         {
         }
         return false;
      }

      // root1 is sealed. SubListAnimation is a public clip of a dynamic class, which gives the core
      // module a path to this object for attachCodeObject.
      private function publishCodeOwner() : void
      {
         if(this.published)
         {
            return;
         }
         this.published = true;
         try
         {
            this.root1.SubListAnimation["PluginSettings"] = this;
         }
         catch(failure:Error)
         {
         }
      }

      private function openWindow() : void
      {
         if(this.windowOpen)
         {
            return;
         }
         this.window.visible = true;
         this.window.prepareToOpen();
         try
         {
            this.activatingBackstop = true;
            this.root1.changeActiveComponent(this.backstop);
            this.activatingBackstop = false;
            this.root1.changeActiveComponent(this.window);
            // The main list stays visible behind a component it does not know.
            this.root1.MainList.visible = false;
            this.windowOpen = true;
            this.showBackHint();
         }
         catch(failure:Error)
         {
            this.activatingBackstop = false;
            this.hideWindow();
            this.root1.MainList.visible = true;
            this.root1.changeActiveComponent(this.root1.MainList);
         }
      }

      // The game's Back made the backstop active. The window is shown again inside that call, so no
      // frame shows the game's menu; onBackedOut decides on the next frame whether it stays.
      private function onBackstopFocused(event:FocusEvent) : void
      {
         if(this.activatingBackstop || !this.windowOpen || this.closePending)
         {
            return;
         }
         this.window.visible = true;
         this.backPending = true;
      }

      private function onBackedOut() : void
      {
         if(this.root1.ActiveComponent !== this.backstop)
         {
            // The game left the window by other means, for example closing the whole menu.
            this.hideWindow();
            return;
         }
         if(getTimer() - this.lastEscapeAt <= ESCAPE_SAME_PRESS_MS)
         {
            this.reactivateWindow();
            return;
         }
         if(this.window.leaveOptions())
         {
            this.reactivateWindow();
            return;
         }
         this.requestClose();
      }

      private function reactivateWindow() : void
      {
         this.window.visible = true;
         this.root1.changeActiveComponent(this.window);
         this.root1.MainList.visible = false;
         this.showBackHint();
      }

      private function onEscapePressed() : void
      {
         this.lastEscapeAt = getTimer();
         if(!this.window.leaveOptions())
         {
            this.requestClose();
         }
         else
         {
            this.playSound("UIMenuMultiplayerBladeClose");
         }
      }

      // Closes once the key press that asked for it is over, so the game's Back or Start of that press
      // cannot leave the main menu.
      private function requestClose() : void
      {
         if(!this.closePending)
         {
            this.closePending = true;
            this.closeRequestedAt = getTimer();
            this.closeDelayFrames = CLOSE_DELAY_FRAMES;
            this.playSound("UIMenuMultiplayerBladeClose");
         }
      }

      private function advanceClose() : void
      {
         if(this.window.isEscapeHeld && getTimer() - this.closeRequestedAt < CLOSE_WAIT_MAX_MS)
         {
            return;
         }
         if(--this.closeDelayFrames > 0)
         {
            return;
         }
         this.closePending = false;
         var active:* = this.root1.ActiveComponent;
         this.hideWindow();
         if(active === this.window || active === this.backstop)
         {
            try
            {
               this.root1.clearPopouts();
               this.root1.changeActiveComponent(this.root1.MainList);
               this.root1.MainList.connectButtonBar();
            }
            catch(failure:Error)
            {
            }
         }
      }

      private function hideWindow() : void
      {
         this.windowOpen = false;
         this.closePending = false;
         this.backPending = false;
         this.window.visible = false;
         this.backstop.visible = false;
         this.window.releaseEscapeListeners();
      }

      // The game's own hint bar, with the game's button texts and key glyphs.
      private function showBackHint() : void
      {
         try
         {
            if(this.backHint == null)
            {
               var hintClass:Class = getDefinitionByName("Shared.AS3.BSButtonHintData") as Class;
               this.backHint = new hintClass("$BACK","Tab","PSN_B","Xenon_B",1,this.onBackHintClicked);
               this.window["buttonHintBarTarget_Inspectable"] = this.root1.ButtonHintBar_mc;
               this.window["AddButtonHintData"](this.backHint);
               this.window["AddButtonHintData"](new hintClass("$ACCEPT","Enter","PSN_A","Xenon_A",1,this.window.acceptSelection));
            }
            this.window["connectButtonBar"]();
         }
         catch(failure:Error)
         {
         }
      }

      private function onBackHintClicked() : void
      {
         if(!this.window.leaveOptions())
         {
            this.requestClose();
         }
      }

      private function playSound(sound:String) : void
      {
         try
         {
            var globalFunc:* = getDefinitionByName("Shared.GlobalFunc");
            globalFunc.PlayMenuSound(sound);
         }
         catch(failure:Error)
         {
         }
      }
   }
}
