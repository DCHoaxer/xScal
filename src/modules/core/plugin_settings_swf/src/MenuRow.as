package
{
   import flash.display.Sprite;
   import flash.filters.GlowFilter;
   import flash.text.TextField;
   import flash.text.TextFieldAutoSize;
   import flash.text.TextFormat;
   import flash.text.TextFormatAlign;
   import flash.utils.getDefinitionByName;

   // One row of the plugin list or the option list, drawn by the game's own SettingsOptionItem with its
   // Option_Checkbox, Option_OptionStepper or Option_Scrollbar. Only the game's OptionsList sends
   // changes to native code (OnSettingChanged), and it is not used here. mouseChildren is off, so the
   // game's controls do not change their own values: PluginSettingsWindow sets them from the plugin's
   // answer.
   public final class MenuRow extends Sprite
   {

      public static const HEIGHT:Number = 48;

      public static const TYPE_NONE:String = "none";

      public static const TYPE_HEADER:String = "header";

      public static const TYPE_TOGGLE:String = "toggle";

      public static const TYPE_CHOICE:String = "choice";

      public static const TYPE_NUMBER:String = "number";

      // SettingsOptionItem.OBJECT_TYPE_*.
      private static const MOVIE_TYPE_SCROLLBAR:uint = 0;

      private static const MOVIE_TYPE_OPTIONSTEPPER:uint = 1;

      private static const MOVIE_TYPE_CHECKBOX:uint = 2;

      // GlobalFunc.COLOR_TEXT_HEADER and COLOR_TEXT_BODY.
      private static const COLOR_HEADER:uint = 0xF5CB5B;

      private static const COLOR_BODY:uint = 0xFFFFCB;

      // The row of the plugin whose options have the focus.
      private static const MARKED_BORDER_ALPHA:Number = 0.35;

      // Option_Scrollbar's track in the row, and the middle of Option_OptionStepper's arrows.
      private static const TRACK_X:Number = 557;

      private static const TRACK_WIDTH:Number = 216;

      private static const STEPPER_MIDDLE_X:Number = 665;

      // The game's slider shows no number; this field right of the track does.
      private static const NUMBER_X:Number = 778;

      private static const NUMBER_WIDTH:Number = 58;

      private var rowWidth:Number;

      // SettingsOptionItem cannot drop a control once it has one, so rows without a control use a
      // second item that never gets one.
      private var plainItem:* = null;

      private var controlItem:* = null;

      private var controlMovieType:int = -1;

      private var numberField:TextField = null;

      private var type:String = TYPE_NONE;

      public function MenuRow(widthOfRow:Number)
      {
         super();
         this.rowWidth = widthOfRow;
         this.mouseChildren = false;
      }

      public function get rowType() : String
      {
         return this.type;
      }

      // Slider fraction 0..1 at this x of the row, or -1 when x is outside the track.
      public function trackFractionAt(rowX:Number) : Number
      {
         if(this.type != TYPE_NUMBER || rowX < TRACK_X - 8 || rowX > TRACK_X + TRACK_WIDTH + 8)
         {
            return -1;
         }
         return Math.max(0,Math.min(1,(rowX - TRACK_X) / TRACK_WIDTH));
      }

      public function stepDirectionAt(rowX:Number) : int
      {
         return rowX < STEPPER_MIDDLE_X ? -1 : 1;
      }

      // controlValue is 0 or 1 for a toggle, the choice index, or the slider fraction 0..1.
      public function show(rowTypeToShow:String, label:String, numberText:String, controlValue:Number, choices:Array, highlighted:Boolean, marked:Boolean) : void
      {
         this.type = rowTypeToShow;
         this.visible = true;
         var hasControl:Boolean = this.type == TYPE_TOGGLE || this.type == TYPE_CHOICE || this.type == TYPE_NUMBER;
         var item:* = hasControl ? this.showControlItem(controlValue,choices) : this.showPlainItem();
         item.selected = highlighted;
         // Equal value and defaultValue keep the game's "modified" asterisk hidden.
         item.SetEntryText({"text":label,"value":controlValue,"defaultValue":controlValue},"");
         if(this.type == TYPE_HEADER)
         {
            item.textField.textColor = COLOR_HEADER;
         }
         if(marked)
         {
            item.border.alpha = MARKED_BORDER_ALPHA;
         }
         this.showNumber(numberText,highlighted);
      }

      private function showPlainItem() : *
      {
         if(this.plainItem == null)
         {
            this.plainItem = this.createItem();
         }
         this.plainItem.visible = true;
         if(this.controlItem != null)
         {
            this.controlItem.visible = false;
         }
         return this.plainItem;
      }

      private function showControlItem(controlValue:Number, choices:Array) : *
      {
         if(this.controlItem == null)
         {
            this.controlItem = this.createItem();
         }
         this.controlItem.visible = true;
         if(this.plainItem != null)
         {
            this.plainItem.visible = false;
         }
         var movieType:int = this.type == TYPE_NUMBER ? int(MOVIE_TYPE_SCROLLBAR) : (this.type == TYPE_CHOICE ? int(MOVIE_TYPE_OPTIONSTEPPER) : int(MOVIE_TYPE_CHECKBOX));
         if(movieType != this.controlMovieType)
         {
            this.controlMovieType = movieType;
            this.controlItem.movieType = movieType;
         }
         if(this.type == TYPE_CHOICE)
         {
            this.controlItem.SetOptionStepperOptions(choices);
         }
         this.controlItem.value = controlValue;
         return this.controlItem;
      }

      private function createItem() : *
      {
         var itemClass:Class = getDefinitionByName("SettingsOptionItem") as Class;
         var item:* = new itemClass();
         // The item is authored for the settings list; the plugin list is narrower. Its dark strip has
         // no instance name, so every child wider than the row is narrowed. The text field shrinks its
         // text to fit, so it needs the full width of the row to stay readable.
         if(this.rowWidth < item.border.width)
         {
            for(var i:int = 0; i < item.numChildren; i++)
            {
               if(item.getChildAt(i).width > this.rowWidth)
               {
                  item.getChildAt(i).width = this.rowWidth;
               }
            }
            item.textField.width = this.rowWidth - item.textField.x - 10;
         }
         this.addChild(item);
         return item;
      }

      private function showNumber(numberText:String, highlighted:Boolean) : void
      {
         if(this.numberField == null)
         {
            if(numberText == "")
            {
               return;
            }
            this.numberField = new TextField();
            this.numberField.autoSize = TextFieldAutoSize.NONE;
            this.numberField.selectable = false;
            this.numberField.mouseEnabled = false;
            this.numberField.embedFonts = false;
            this.numberField.x = NUMBER_X;
            this.numberField.y = 10;
            this.numberField.width = NUMBER_WIDTH;
            this.numberField.height = 28;
            this.addChild(this.numberField);
         }
         this.numberField.visible = numberText != "";
         if(numberText == "")
         {
            return;
         }
         // FO76 draws boxes unless setTextFormat follows every text change.
         var format:TextFormat = new TextFormat("$MAIN_Font_Bold",19,highlighted ? 0 : COLOR_BODY,true);
         format.align = TextFormatAlign.RIGHT;
         this.numberField.defaultTextFormat = format;
         this.numberField.text = numberText;
         this.numberField.setTextFormat(format,0,this.numberField.length);
         this.numberField.filters = highlighted ? [] : [new GlowFilter(0,1,2,2,4,1)];
      }
   }
}
