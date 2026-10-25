		bool m_fPopup = false;
		int m_PopupMenu = -1;

	public:
		// サブメニュー項目の位置
		enum {
			SUBMENU_ZOOM           = 0,
			SUBMENU_ASPECTRATIO    = 1,
			SUBMENU_CHANNEL        = 5,
			SUBMENU_SERVICE        = 6,
			SUBMENU_SPACE          = 7,
			SUBMENU_FAVORITES      = 8,
			SUBMENU_CHANNELHISTORY = 9,
			SUBMENU_VOLUME         = 12,
			SUBMENU_AUDIO          = 13,
			SUBMENU_VIDEO          = 14,
			SUBMENU_RESET          = 25,
			SUBMENU_BAR            = 29,
			SUBMENU_PLUGIN         = 30,
			SUBMENU_FILTERPROPERTY = 32
		};