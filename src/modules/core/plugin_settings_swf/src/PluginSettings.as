package
{
   import Shared.AS3.MenuComponent;
   import com.brokenfunction.json.decodeJson;
   import flash.display.DisplayObject;
   import flash.display.Sprite;
   import flash.events.Event;
   import flash.events.FocusEvent;
   import flash.text.TextField;
   import flash.text.TextFieldType;
   import flash.utils.getDefinitionByName;
   import flash.utils.getTimer;

   // Document class of PluginSettings.swf, loaded into seventysixmenu.swf by xScal's core module. Adds the
   // main menu entry, opens the settings window from it and the xScal credits on X (keyboard or pad),
   // each as a component of the game's menu stack. One of the two is open at a time; "layer" below
   // means that one.
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

      private var credits:XScalCredits;

      // The open window or credits, null when neither is open.
      private var layer:* = null;

      private var input:KeyInput;

      // Starts true, so an X held when the menu loads does not open the credits.
      private var creditsKeyHeld:Boolean = true;

      // Children of root1 that the credits hid, shown again when they close.
      private var hiddenByCredits:Array = [];

      // The active component while the game's Back runs: Back activates the previous component in its
      // history, and this one keeps the main list out of that step.
      private var backstop:MenuComponent;

      private var mainMenu:MainMenuLink;

      private var root1:* = null;

      private var frameCount:int = 0;

      private var published:Boolean = false;

      private var activatingBackstop:Boolean = false;

      private var backPending:Boolean = false;

      private var closePending:Boolean = false;

      private var closeRequestedAt:int = 0;

      private var closeDelayFrames:int = 0;

      private var lastEscapeAt:int = -100000;

      private var hintsAdded:Array = [];

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
         this.input = new KeyInput(this);
         this.credits = new XScalCredits();
         this.credits.onEscape = this.onEscapePressed;
         this.credits.input = this.input;
         this.credits.addEventListener(XScalCredits.FINISHED,this.onCreditsFinished);
         this.addChild(this.credits);
         this.mainMenu = new MainMenuLink(this.openWindow,this.openCredits);
         this.mainMenu.setEntryText(Texts.get("entry"));
         this.mainMenu.setCreditsHintText(Texts.get("credits_hint"));
         this.addEventListener(Event.ENTER_FRAME,this.onEnterFrame);
      }

      // Read by the core module every frame: mainmenu.swf draws the news panel under the window and
      // the credits.
      public function get isWindowOpen() : Boolean
      {
         return this.layer != null;
      }

      // Read by the core module every frame: it plays the credits music while this is true.
      public function get isCreditsOpen() : Boolean
      {
         return this.layer === this.credits;
      }

      // Set by the core module once the SWF has loaded: src/modules/core/credits.txt from the DLL.
      public function set creditsText(text:String) : void
      {
         this.credits.text = text;
      }

      // Set by the core module once the SWF has loaded.
      public function set language(code:String) : void
      {
         Texts.language = code;
         this.mainMenu.setEntryText(Texts.get("entry"));
         this.mainMenu.setCreditsHintText(Texts.get("credits_hint"));
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
         this.readCreditsKey();
         if(this.layer === this.credits)
         {
            this.hideMenuForCredits();
         }
         if(this.closePending)
         {
            this.advanceClose();
         }
         else if(this.layer != null && (this.backPending || !this.layer.visible || !this.layer.Active))
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
         this.openLayer(this.window);
      }

      private function openLayer(opened:*) : void
      {
         if(this.layer != null)
         {
            return;
         }
         this.layer = opened;
         opened.visible = true;
         opened.prepareToOpen();
         try
         {
            this.activatingBackstop = true;
            this.root1.changeActiveComponent(this.backstop);
            this.activatingBackstop = false;
            this.root1.changeActiveComponent(opened);
            // The main list stays visible behind a component it does not know.
            this.root1.MainList.visible = false;
            if(opened === this.credits)
            {
               this.hideMenuForCredits();
            }
            this.showBackHint();
         }
         catch(failure:Error)
         {
            this.activatingBackstop = false;
            this.hideLayer();
            this.root1.MainList.visible = true;
            this.root1.changeActiveComponent(this.root1.MainList);
         }
      }

      // X on the keyboard or the pad opens the credits from the main list. On PC the game gives the main
      // list no action for either: its XButton is R on the keyboard, and pad X only changes the user on
      // Xbox.
      private function readCreditsKey() : void
      {
         if(!this.input.register())
         {
            return;
         }
         // Not read while a window is open; the X that is held when it closes counts as held.
         if(this.layer != null)
         {
            this.creditsKeyHeld = true;
            return;
         }
         var down:Boolean = this.input.isKeyDown(KeyInput.KEY_X) || this.input.isPadDown(KeyInput.PAD_X);
         var pressed:Boolean = down && !this.creditsKeyHeld;
         this.creditsKeyHeld = down;
         if(!pressed)
         {
            return;
         }
         try
         {
            var focused:TextField = this.stage != null ? this.stage.focus as TextField : null;
            if(focused != null && focused.type == TextFieldType.INPUT)
            {
               return;
            }
         }
         catch(failure:Error)
         {
            return;
         }
         this.openCredits();
      }

      // From X and from a click on the hint.
      private function openCredits() : void
      {
         if(this.layer != null || this.closePending || this.root1 == null)
         {
            return;
         }
         try
         {
            if(this.root1.ActiveComponent !== this.root1.MainList || !this.root1.MainList.visible)
            {
               return;
            }
         }
         catch(failure:Error)
         {
            return;
         }
         this.playSound("UIMenuOK");
         this.openLayer(this.credits);
      }

      private function onCreditsFinished(event:Event) : void
      {
         if(this.layer === this.credits)
         {
            this.requestClose();
         }
      }

      // The credits show alone over the menu background. Runs every frame while they are open, because
      // the game shows some of these objects again on its own updates.
      private function hideMenuForCredits() : void
      {
         try
         {
            var own:DisplayObject = this;
            while(own != null && own.parent !== this.root1)
            {
               own = own.parent;
            }
            var hintBar:* = this.root1.ButtonHintBar_mc;
            var count:int = int(this.root1.numChildren);
            for(var i:int = 0; i < count; i++)
            {
               var child:DisplayObject = this.root1.getChildAt(i);
               if(child === own || child === hintBar || !child.visible)
               {
                  continue;
               }
               child.visible = false;
               this.hiddenByCredits.push(child);
            }
         }
         catch(failure:Error)
         {
         }
      }

      // Shows every object the credits hid. The game's change back to the main list sets that list's
      // visibility itself.
      private function showMenuAfterCredits() : void
      {
         for each(var child:DisplayObject in this.hiddenByCredits)
         {
            child.visible = true;
         }
         this.hiddenByCredits = [];
      }

      // The game's Back made the backstop active. The window is shown again inside that call, so no
      // frame shows the game's menu; onBackedOut decides on the next frame whether it stays.
      private function onBackstopFocused(event:FocusEvent) : void
      {
         if(this.activatingBackstop || this.layer == null || this.closePending)
         {
            return;
         }
         this.layer.visible = true;
         this.backPending = true;
      }

      private function onBackedOut() : void
      {
         if(this.root1.ActiveComponent !== this.backstop)
         {
            // The game left the window by other means, for example closing the whole menu.
            this.hideLayer();
            return;
         }
         if(getTimer() - this.lastEscapeAt <= ESCAPE_SAME_PRESS_MS)
         {
            this.reactivateWindow();
            return;
         }
         if(this.layer.leaveOptions())
         {
            this.reactivateWindow();
            return;
         }
         this.requestClose();
      }

      private function reactivateWindow() : void
      {
         this.layer.visible = true;
         this.root1.changeActiveComponent(this.layer);
         this.root1.MainList.visible = false;
         this.showBackHint();
      }

      private function onEscapePressed() : void
      {
         this.lastEscapeAt = getTimer();
         if(this.layer == null)
         {
            return;
         }
         if(!this.layer.leaveOptions())
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
         if(this.layer != null && this.layer.isEscapeHeld && getTimer() - this.closeRequestedAt < CLOSE_WAIT_MAX_MS)
         {
            return;
         }
         if(--this.closeDelayFrames > 0)
         {
            return;
         }
         this.closePending = false;
         var active:* = this.root1.ActiveComponent;
         var closed:* = this.layer;
         this.hideLayer();
         if((closed != null && active === closed) || active === this.backstop)
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

      private function hideLayer() : void
      {
         var hidden:* = this.layer;
         this.layer = null;
         this.closePending = false;
         this.backPending = false;
         this.backstop.visible = false;
         if(hidden === this.credits)
         {
            this.showMenuAfterCredits();
         }
         if(hidden != null)
         {
            hidden.visible = false;
            hidden.releaseEscapeListeners();
         }
      }

      // The game's own hint bar, with the game's button texts and key glyphs. The credits get only Back,
      // like the game's credits.
      private function showBackHint() : void
      {
         try
         {
            if(this.hintsAdded.indexOf(this.layer) < 0)
            {
               var hintClass:Class = getDefinitionByName("Shared.AS3.BSButtonHintData") as Class;
               this.layer["buttonHintBarTarget_Inspectable"] = this.root1.ButtonHintBar_mc;
               this.layer["AddButtonHintData"](new hintClass("$BACK","Tab","PSN_B","Xenon_B",1,this.onBackHintClicked));
               if(this.layer === this.window)
               {
                  this.layer["AddButtonHintData"](new hintClass("$ACCEPT","Enter","PSN_A","Xenon_A",1,this.window.acceptSelection));
               }
               this.hintsAdded.push(this.layer);
            }
            this.layer["connectButtonBar"]();
         }
         catch(failure:Error)
         {
         }
      }

      private function onBackHintClicked() : void
      {
         if(this.layer != null && !this.layer.leaveOptions())
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
