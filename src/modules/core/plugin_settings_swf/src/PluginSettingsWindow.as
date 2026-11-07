package
{
   import Shared.AS3.MenuComponent;
   import flash.display.Shape;
   import flash.display.Stage;
   import flash.events.KeyboardEvent;
   import flash.events.MouseEvent;
   import flash.filters.GlowFilter;
   import flash.text.TextField;
   import flash.text.TextFormat;
   import flash.ui.Keyboard;
   import flash.utils.getDefinitionByName;
   import flash.utils.getTimer;

   // Plugin list on the left, the options of the selected plugin on the right, at the positions of
   // the game's SubList and SettingsList_mc. The window is a MenuComponent so the game's
   // changeActiveComponent gives it stage focus and its Back reaches PluginSettings.
   public class PluginSettingsWindow extends MenuComponent
   {

      // SubListAnimation and SettingsList_mc on the root of seventysixmenu.swf.
      private static const PLUGINS_X:Number = 190;

      private static const PLUGINS_Y:Number = 187;

      private static const PLUGINS_WIDTH:Number = 390;

      // SubListStyle.verticalSpacing_Inspectable.
      private static const PLUGIN_ROW_SPACING:Number = 8;

      private static const OPTIONS_X:Number = 612.75;

      private static const OPTIONS_Y:Number = 308.65;

      private static const OPTIONS_WIDTH:Number = 842;

      private static const VISIBLE_PLUGINS:int = 13;

      private static const VISIBLE_OPTIONS:int = 13;

      private static const TITLE_COLOR:uint = 0xF5CB5B;

      // The Enter that accepted the main menu entry can reach the window as well.
      private static const ENTER_IGNORE_MS:int = 400;

      private static const SOUND_MOVE:String = "UIMenuFocus";

      private static const SOUND_ACCEPT:String = "UIMenuOK";

      private static const SOUND_CHANGE:String = "UIMenuPrevNext";

      private static const SOUND_SLIDER:String = "UIMenuQuantity";

      public var readPlugins:Function;

      // (pluginId, optionId, value): true when the plugin applied the value.
      public var writeOption:Function;

      public var onEscape:Function;

      private var plugins:Array = [];

      private var pluginIndex:int = 0;

      private var pluginScroll:int = 0;

      private var options:Array = [];

      private var optionIndex:int = 0;

      private var optionScroll:int = 0;

      private var hasOptionFocus:Boolean = false;

      private var pluginRows:Vector.<MenuRow> = new Vector.<MenuRow>();

      private var optionRows:Vector.<MenuRow> = new Vector.<MenuRow>();

      private var titleField:TextField;

      private var titleRule:Shape = new Shape();

      private var enterAcceptedAt:int = 0;

      private var listenedStage:Stage = null;

      private var escapeHeld:Boolean = false;

      public function PluginSettingsWindow()
      {
         super();
         this.visible = false;
         var i:int;
         for(i = 0; i < VISIBLE_PLUGINS; i++)
         {
            var pluginRow:MenuRow = new MenuRow(PLUGINS_WIDTH);
            pluginRow.x = PLUGINS_X;
            pluginRow.y = PLUGINS_Y + i * (MenuRow.HEIGHT + PLUGIN_ROW_SPACING);
            pluginRow.addEventListener(MouseEvent.MOUSE_OVER,this.onPluginRowOver);
            pluginRow.addEventListener(MouseEvent.CLICK,this.onPluginRowClick);
            this.addChild(pluginRow);
            this.pluginRows.push(pluginRow);
         }
         for(i = 0; i < VISIBLE_OPTIONS; i++)
         {
            var optionRow:MenuRow = new MenuRow(OPTIONS_WIDTH);
            optionRow.x = OPTIONS_X;
            optionRow.y = OPTIONS_Y + i * MenuRow.HEIGHT;
            optionRow.addEventListener(MouseEvent.MOUSE_OVER,this.onOptionRowOver);
            optionRow.addEventListener(MouseEvent.CLICK,this.onOptionRowClick);
            this.addChild(optionRow);
            this.optionRows.push(optionRow);
         }
         this.addChild(this.titleRule);
         this.titleField = new TextField();
         this.titleField.selectable = false;
         this.titleField.mouseEnabled = false;
         this.titleField.embedFonts = false;
         this.titleField.x = OPTIONS_X + 33.5;
         this.titleField.y = OPTIONS_Y - 54;
         this.titleField.width = OPTIONS_WIDTH - 33.5;
         this.titleField.height = 40;
         this.titleField.filters = [new GlowFilter(0,1,2,2,4,1)];
         this.addChild(this.titleField);
         this.addEventListener(KeyboardEvent.KEY_DOWN,this.onKeyDown);
         this.addEventListener(MouseEvent.MOUSE_WHEEL,this.onMouseWheel);
      }

      // The game maps Tab and pad B to Back; PluginSettings calls this for both and for Esc. Returns
      // true when the option list lost the focus, false when the plugin list already had it.
      public function leaveOptions() : Boolean
      {
         if(!this.hasOptionFocus)
         {
            return false;
         }
         this.hasOptionFocus = false;
         this.draw();
         return true;
      }

      public function get isEscapeHeld() : Boolean
      {
         return this.escapeHeld;
      }

      public function prepareToOpen() : void
      {
         this.enterAcceptedAt = getTimer() + ENTER_IGNORE_MS;
         this.plugins = [];
         this.options = [];
         this.pluginIndex = 0;
         this.pluginScroll = 0;
         this.hasOptionFocus = false;
         this.reload();
         this.addEscapeListeners();
      }

      public function releaseEscapeListeners() : void
      {
         if(this.listenedStage == null)
         {
            return;
         }
         this.listenedStage.removeEventListener(KeyboardEvent.KEY_DOWN,this.onStageKeyDown,true);
         this.listenedStage.removeEventListener(KeyboardEvent.KEY_DOWN,this.onStageKeyDown,false);
         // The key-up listeners stay until the Esc release has been swallowed.
         if(!this.escapeHeld)
         {
            this.listenedStage.removeEventListener(KeyboardEvent.KEY_UP,this.onStageKeyUp,true);
            this.listenedStage.removeEventListener(KeyboardEvent.KEY_UP,this.onStageKeyUp,false);
         }
      }

      // Esc is caught on the stage: stopping it keeps the game's Cancel (sent on key release) from
      // backing out of the main menu. Both phases, because the event target can be the stage itself.
      private function addEscapeListeners() : void
      {
         this.listenedStage = this.stage;
         if(this.listenedStage == null)
         {
            return;
         }
         this.listenedStage.addEventListener(KeyboardEvent.KEY_DOWN,this.onStageKeyDown,true,int.MAX_VALUE);
         this.listenedStage.addEventListener(KeyboardEvent.KEY_DOWN,this.onStageKeyDown,false,int.MAX_VALUE);
         this.listenedStage.addEventListener(KeyboardEvent.KEY_UP,this.onStageKeyUp,true,int.MAX_VALUE);
         this.listenedStage.addEventListener(KeyboardEvent.KEY_UP,this.onStageKeyUp,false,int.MAX_VALUE);
      }

      private function onStageKeyDown(event:KeyboardEvent) : void
      {
         if(event.keyCode != Keyboard.ESCAPE || !this.visible)
         {
            return;
         }
         event.stopImmediatePropagation();
         if(this.escapeHeld)
         {
            return;
         }
         this.escapeHeld = true;
         this.onEscape();
      }

      private function onStageKeyUp(event:KeyboardEvent) : void
      {
         if(event.keyCode != Keyboard.ESCAPE || !this.escapeHeld)
         {
            return;
         }
         event.stopImmediatePropagation();
         this.escapeHeld = false;
         if(!this.visible && this.listenedStage != null)
         {
            this.listenedStage.removeEventListener(KeyboardEvent.KEY_UP,this.onStageKeyUp,true);
            this.listenedStage.removeEventListener(KeyboardEvent.KEY_UP,this.onStageKeyUp,false);
         }
      }

      private function onKeyDown(event:KeyboardEvent) : void
      {
         switch(event.keyCode)
         {
            case Keyboard.UP:
               this.moveSelection(-1);
               break;
            case Keyboard.DOWN:
               this.moveSelection(1);
               break;
            case Keyboard.LEFT:
               if(this.hasOptionFocus)
               {
                  this.stepSelectedOption(-1);
               }
               break;
            case Keyboard.RIGHT:
               if(this.hasOptionFocus)
               {
                  this.stepSelectedOption(1);
               }
               else
               {
                  this.enterOptions();
               }
               break;
            case Keyboard.ENTER:
               if(getTimer() >= this.enterAcceptedAt)
               {
                  this.acceptSelection();
               }
         }
      }

      // Enter, pad A and the Accept hint.
      public function acceptSelection() : void
      {
         if(this.hasOptionFocus)
         {
            this.acceptSelectedOption();
         }
         else
         {
            this.enterOptions();
         }
      }

      private function onMouseWheel(event:MouseEvent) : void
      {
         this.moveSelection(event.delta > 0 ? -1 : 1);
      }

      private function moveSelection(direction:int) : void
      {
         if(this.hasOptionFocus)
         {
            var next:int = this.nextSelectableOption(this.optionIndex,direction);
            if(next == this.optionIndex)
            {
               return;
            }
            this.optionIndex = next;
            this.scrollOptionsToSelection();
         }
         else
         {
            if(this.plugins.length < 2)
            {
               return;
            }
            this.pluginIndex = (this.pluginIndex + direction + this.plugins.length) % this.plugins.length;
            this.scrollPluginsToSelection();
            this.showOptionsOfSelectedPlugin();
         }
         this.playSound(SOUND_MOVE);
         this.draw();
      }

      private function nextSelectableOption(from:int, direction:int) : int
      {
         var count:int = this.options.length;
         for(var step:int = 1; step <= count; step++)
         {
            var candidate:int = (from + direction * step + count * step) % count;
            if(this.isSelectable(this.options[candidate]))
            {
               return candidate;
            }
         }
         return from;
      }

      private function isSelectable(option:Object) : Boolean
      {
         return option != null && option.type != MenuRow.TYPE_HEADER;
      }

      private function enterOptions() : void
      {
         if(this.options.length == 0)
         {
            return;
         }
         this.hasOptionFocus = true;
         if(!this.isSelectable(this.options[this.optionIndex]))
         {
            this.optionIndex = this.nextSelectableOption(this.optionIndex,1);
         }
         this.playSound(SOUND_ACCEPT);
         this.draw();
      }

      private function scrollPluginsToSelection() : void
      {
         if(this.pluginIndex < this.pluginScroll)
         {
            this.pluginScroll = this.pluginIndex;
         }
         else if(this.pluginIndex >= this.pluginScroll + VISIBLE_PLUGINS)
         {
            this.pluginScroll = this.pluginIndex - VISIBLE_PLUGINS + 1;
         }
      }

      private function scrollOptionsToSelection() : void
      {
         // A group header directly above the selection scrolls into view with it.
         var top:int = this.optionIndex;
         if(top > 0 && !this.isSelectable(this.options[top - 1]))
         {
            top--;
         }
         if(top < this.optionScroll)
         {
            this.optionScroll = top;
         }
         else if(this.optionIndex >= this.optionScroll + VISIBLE_OPTIONS)
         {
            this.optionScroll = this.optionIndex - VISIBLE_OPTIONS + 1;
         }
      }

      // Reads the list again: plugins may list different rows after a change. The selection stays on
      // the same plugin and option when they are still listed.
      private function reload() : void
      {
         var selectedPlugin:String = this.plugins.length > this.pluginIndex ? String(this.plugins[this.pluginIndex].id) : "";
         var selectedOption:String = this.options.length > this.optionIndex && this.options[this.optionIndex] != null ? String(this.options[this.optionIndex].id) : "";
         var previousScroll:int = this.optionScroll;
         this.plugins = this.pluginsWithOptions(this.readPlugins());
         this.pluginIndex = 0;
         var i:int;
         for(i = 0; i < this.plugins.length; i++)
         {
            if(String(this.plugins[i].id) == selectedPlugin)
            {
               this.pluginIndex = i;
            }
         }
         this.scrollPluginsToSelection();
         this.showOptionsOfSelectedPlugin();
         for(i = 0; i < this.options.length; i++)
         {
            if(String(this.options[i].id) == selectedOption && this.isSelectable(this.options[i]))
            {
               this.optionIndex = i;
               this.optionScroll = previousScroll;
            }
         }
         this.scrollOptionsToSelection();
         if(this.options.length == 0)
         {
            this.hasOptionFocus = false;
         }
         this.draw();
      }

      // A plugin without options has nothing to show.
      private function pluginsWithOptions(listed:Array) : Array
      {
         var result:Array = [];
         for each(var plugin:Object in listed)
         {
            if(plugin != null && plugin.options is Array && plugin.options.length > 0)
            {
               result.push(plugin);
            }
         }
         return result;
      }

      // The group of an option becomes a header row above its first option.
      private function showOptionsOfSelectedPlugin() : void
      {
         this.options = [];
         this.optionScroll = 0;
         if(this.plugins.length == 0)
         {
            this.optionIndex = 0;
            return;
         }
         var previousGroup:String = "";
         for each(var option:Object in this.plugins[this.pluginIndex].options)
         {
            if(option == null)
            {
               continue;
            }
            var group:String = option.group != null ? String(option.group) : "";
            if(group != "" && group != previousGroup)
            {
               this.options.push({"id":"","type":MenuRow.TYPE_HEADER,"label":group});
            }
            previousGroup = group;
            this.options.push(option);
         }
         this.optionIndex = 0;
         if(this.options.length > 0 && !this.isSelectable(this.options[0]))
         {
            this.optionIndex = this.nextSelectableOption(0,1);
         }
      }

      private function draw() : void
      {
         var i:int;
         for(i = 0; i < this.pluginRows.length; i++)
         {
            var pluginAt:int = this.pluginScroll + i;
            if(pluginAt < this.plugins.length)
            {
               var isCurrent:Boolean = pluginAt == this.pluginIndex;
               this.pluginRows[i].show(MenuRow.TYPE_NONE,String(this.plugins[pluginAt].name),"",0,null,isCurrent && !this.hasOptionFocus,isCurrent && this.hasOptionFocus);
            }
            else if(i == 0)
            {
               this.pluginRows[i].show(MenuRow.TYPE_NONE,Texts.get("no_plugins"),"",0,null,false,false);
            }
            else
            {
               this.pluginRows[i].visible = false;
            }
         }
         for(i = 0; i < this.optionRows.length; i++)
         {
            var optionAt:int = this.optionScroll + i;
            if(optionAt < this.options.length)
            {
               this.showOption(this.optionRows[i],this.options[optionAt],this.hasOptionFocus && optionAt == this.optionIndex);
            }
            else
            {
               this.optionRows[i].visible = false;
            }
         }
         this.drawTitle();
      }

      private function showOption(row:MenuRow, option:Object, highlighted:Boolean) : void
      {
         switch(String(option.type))
         {
            case MenuRow.TYPE_HEADER:
               row.show(MenuRow.TYPE_HEADER,String(option.label),"",0,null,false,false);
               break;
            case MenuRow.TYPE_CHOICE:
               var choices:Array = option.choices is Array ? option.choices as Array : [];
               row.show(MenuRow.TYPE_CHOICE,String(option.label),"",int(option.value),choices,highlighted,false);
               break;
            case MenuRow.TYPE_NUMBER:
               var minimum:Number = Number(option.min);
               var maximum:Number = Number(option.max);
               var value:Number = Number(option.value);
               var decimals:int = Math.max(0,Math.min(6,int(option.decimals)));
               var fraction:Number = maximum > minimum ? (value - minimum) / (maximum - minimum) : 0;
               row.show(MenuRow.TYPE_NUMBER,String(option.label),value.toFixed(decimals),Math.max(0,Math.min(1,fraction)),null,highlighted,false);
               break;
            default:
               row.show(MenuRow.TYPE_TOGGLE,String(option.label),"",isOn(option.value) ? 1 : 0,null,highlighted,false);
         }
      }

      private function drawTitle() : void
      {
         var hasPlugin:Boolean = this.plugins.length > 0;
         this.titleField.visible = hasPlugin;
         this.titleRule.visible = hasPlugin;
         if(!hasPlugin)
         {
            return;
         }
         var format:TextFormat = new TextFormat("$MAIN_Font_Bold",28,TITLE_COLOR,true);
         this.titleField.defaultTextFormat = format;
         this.titleField.text = String(this.plugins[this.pluginIndex].name).toUpperCase();
         this.titleField.setTextFormat(format,0,this.titleField.length);
         this.titleRule.graphics.clear();
         this.titleRule.graphics.lineStyle(2,TITLE_COLOR,0.8);
         this.titleRule.graphics.moveTo(OPTIONS_X,OPTIONS_Y - 10);
         this.titleRule.graphics.lineTo(OPTIONS_X + OPTIONS_WIDTH,OPTIONS_Y - 10);
      }

      private static function isOn(value:*) : Boolean
      {
         return value === true || value === "true" || Number(value) != 0 && !isNaN(Number(value));
      }

      private function stepSelectedOption(direction:int) : void
      {
         var option:Object = this.options[this.optionIndex];
         if(!this.isSelectable(option))
         {
            return;
         }
         switch(String(option.type))
         {
            case MenuRow.TYPE_NUMBER:
               this.applyNumber(option,Number(option.value) + Number(option.step) * direction);
               break;
            case MenuRow.TYPE_CHOICE:
               var count:int = option.choices is Array ? int(option.choices.length) : 0;
               if(count > 0)
               {
                  this.apply(option,(int(option.value) + direction + count) % count,SOUND_CHANGE);
               }
               break;
            default:
               this.apply(option,!isOn(option.value),SOUND_CHANGE);
         }
      }

      // Enter does nothing on a slider, the same as on the game's Option_Scrollbar.
      private function acceptSelectedOption() : void
      {
         var option:Object = this.options[this.optionIndex];
         if(this.isSelectable(option) && String(option.type) != MenuRow.TYPE_NUMBER)
         {
            this.stepSelectedOption(1);
         }
      }

      // Rounds to whole steps from the minimum, so repeated presses stay on the option's grid.
      private function applyNumber(option:Object, wanted:Number) : void
      {
         var minimum:Number = Number(option.min);
         var maximum:Number = Number(option.max);
         var step:Number = Number(option.step) > 0 ? Number(option.step) : 1;
         var next:Number = minimum + Math.round((wanted - minimum) / step) * step;
         next = Number(Math.max(minimum,Math.min(maximum,next)).toFixed(6));
         if(next != Number(option.value))
         {
            this.apply(option,next,SOUND_SLIDER);
         }
      }

      private function apply(option:Object, value:*, sound:String) : void
      {
         this.writeOption(String(this.plugins[this.pluginIndex].id),String(option.id),value);
         this.playSound(sound);
         this.reload();
      }

      private function onPluginRowOver(event:MouseEvent) : void
      {
         var at:int = this.pluginScroll + this.pluginRows.indexOf(event.currentTarget as MenuRow);
         if(at >= this.plugins.length || at == this.pluginIndex && !this.hasOptionFocus)
         {
            return;
         }
         var changed:Boolean = at != this.pluginIndex;
         this.hasOptionFocus = false;
         this.pluginIndex = at;
         if(changed)
         {
            this.showOptionsOfSelectedPlugin();
            this.playSound(SOUND_MOVE);
         }
         this.draw();
      }

      private function onPluginRowClick(event:MouseEvent) : void
      {
         this.focusWindow();
         this.enterOptions();
      }

      private function onOptionRowOver(event:MouseEvent) : void
      {
         var at:int = this.optionScroll + this.optionRows.indexOf(event.currentTarget as MenuRow);
         if(at >= this.options.length || !this.isSelectable(this.options[at]) || at == this.optionIndex && this.hasOptionFocus)
         {
            return;
         }
         this.hasOptionFocus = true;
         this.optionIndex = at;
         this.playSound(SOUND_MOVE);
         this.draw();
      }

      private function onOptionRowClick(event:MouseEvent) : void
      {
         this.focusWindow();
         var row:MenuRow = event.currentTarget as MenuRow;
         var at:int = this.optionScroll + this.optionRows.indexOf(row);
         if(at >= this.options.length || !this.isSelectable(this.options[at]))
         {
            return;
         }
         var option:Object = this.options[at];
         this.optionIndex = at;
         this.hasOptionFocus = true;
         if(row.rowType == MenuRow.TYPE_NUMBER)
         {
            var fraction:Number = row.trackFractionAt(event.localX);
            if(fraction >= 0)
            {
               this.applyNumber(option,Number(option.min) + (Number(option.max) - Number(option.min)) * fraction);
            }
            else
            {
               this.draw();
            }
         }
         else
         {
            this.stepSelectedOption(row.stepDirectionAt(event.localX));
         }
      }

      // A click moves the stage focus away from the window, and keys arrive at the focus.
      private function focusWindow() : void
      {
         if(this.stage != null)
         {
            this.stage.focus = this;
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
