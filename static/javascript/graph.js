// ==============================================
// Copyright 2012 by Chris Valleskey
// Source: chrisvalleskey.com/html5-graph/
// Author: Chris Valleskey
// Do not distribute without authorization.
// ==============================================

var Graph = function(options)
{
	var default_args = {
		'id': "graph",
		'interval':	300,
		'showline':	true,
		'showfill':	true,
		'lineWidth':	2,
		'strokeStyle':	"#666",
		'gridcolor': "#EEE",
		'background': "#F9F9F9",
		'fillStyle':	"rgba(0,0,0,0.25)",
		'showdots':	true,
		'showgrid': true,
		'showlabels': true,
		'grid': [10,10],
		'range': [0,100],
		'call': function(){return Math.floor(Math.random()*100) + 50;}
	}
	for(var index in default_args) {
		if(typeof options[index] == "undefined")
			options[index] = default_args[index];
	}

	this.id = options['id'];
	this.interval = options['interval'];
	this.lineWidth = options['lineWidth'];
	this.strokeStyle = options['strokeStyle'];
	this.fillStyle = options['fillStyle'];
	this.gridcolor = options['gridcolor'];
	this.background = options['background'];
	this.showdots = options['showdots'];
	this.showshadow = options['showshadow'];
	this.showgrid = options['showgrid'];
	this.showline = options['showline'];
	this.showfill = options['showfill'];
	this.range = options['range'];
	this.call = options['call'];
	this.data = options['data'];
	this.canvas = document.getElementById(this.id);
	this.context = document.getElementById(this.id).getContext("2d");	
	this.maxvalue = this.range[1] - this.range[0];
	this.scale = Math.round((this.canvas.height / this.maxvalue)*10)/10;
	this.showlabels = options['showlabels'];
	this.labelfilter = options['labelfilter'];
	if(options['grid'].constructor.toString().indexOf("Array") == -1)
	{
		this.grid_x = this.grid_y = Math.floor(options['grid'] * this.scale);
	} else {
		this.grid_x = Math.floor(options['grid'][0] * this.scale);
		this.grid_y = Math.floor(options['grid'][1] * this.scale);
	}
	this.array = [];

	var _self = this;

	this.drawGraph = function()
	{
		this.canvas.setAttribute('width',this.canvas.width); // Clear the canvas
		this.context.fillStyle = this.background;
		this.context.fillRect(0,0,this.canvas.width,this.canvas.height); // Fill the background

		if(this.showgrid)
			this.drawGrid();

		this.context.lineWidth   = this.lineWidth;
		this.context.strokeStyle = this.strokeStyle;
		this.context.fillStyle = this.fillStyle;
		this.context.lineJoin = "round";

		if(this.showshadow)
		{
			this.context.shadowColor = this.fillStyle;
			this.context.shadowBlur = 5;
		}

		if(this.array.length > (this.canvas.width/this.grid_x)+1)
			this.array.shift();

		if(this.showfill)
			this.drawFill();
		if(this.showline && this.lineWidth > 0)
			this.drawLine();
		if(this.showdots)
			this.drawDots();
		if(this.showlabels)
			this.drawLabels();
	}		
	this.doGraph = function()
	{	
		call = this.call();

		if(call == undefined)
			call = 0;

		value = Math.round((this.range[1] - call ) * this.scale);

		this.array.push(value);

		if(options['debug'])
			document.getElementById('output').innerHTML = "CALL: " + call + " VALUE: " + value + " SCALE: " + this.scale;

		this.drawGraph();
	}
	this.drawFill = function()
	{
		this.context.beginPath();
		for(var i = 0; i < this.array.length; i++)
		{
			var pos_y = this.array[i];

			this.context.lineTo(i*this.grid_x+0.5, pos_y);
		}

		this.context.lineTo((i-1)*this.grid_x+0.5, this.canvas.height-0.5);
		this.context.lineTo(0, this.canvas.height);

		if(this.array.length > 1)
			this.context.fill();

		this.context.closePath();
	}
	this.drawLine = function()
	{
		this.context.beginPath();
		var offset = ((this.context.lineWidth+1)%2)/2;
		for(var i = 0; i < this.array.length; i++)
		{
			var pos_y = this.array[i];

			this.context.lineTo(i*this.grid_x+offset, pos_y);
		}
		this.context.stroke();
		this.context.closePath();
	}
	this.drawDots = function()
	{
		this.context.beginPath();
			this.context.fillStyle = this.strokeStyle;
			for(var i = 0; i < this.array.length; i++)
			{			
				var pos_y = this.array[i];
				var dotradius = 3;

				this.context.beginPath();
				this.context.arc(i*this.grid_x+0.5, pos_y-0.5,dotradius,0,Math.PI*2,true);	
				this.context.fill();
				this.context.closePath();
			}		
		this.context.closePath();
	}
	this.drawLabels = function()
	{
			for(var i = 0; i < this.array.length; i++)
			{			
				var pos_y = this.array[i];

				if(this.data != undefined)
					text = this.data[i];
				else
					text = Math.round(((this.array[i] / this.scale) - this.range[1]) * -1);

				this.drawLabel(text,i*this.grid_x+0.5, pos_y-0.5);
			}		
	}
	this.drawLabel = function(text,pos_x,pos_y)
	{
		this.context.save();

		if(pos_y < 20)
		{
			if(pos_y > 0)
				pos_y += 15;
			else
				pos_y = 13;
		} else if(pos_y > this.canvas.height)
		{
			pos_y = this.canvas.height - 10;
		}
		else
		{
			pos_y -= 10;
		}

		if(this.labelfilter != undefined)
			text = this.labelfilter.replace(/%label%/g,text);

		this.context.textAlign = "center";
		this.context.font = 'bold 10px/2 sans-serif';
		this.context.fillStyle = "#333";
		this.context.fillText(text,pos_x,pos_y);
		this.context.restore();
	}
	this.drawGrid = function()
	{
		this.context.beginPath();
		if(this.grid_x > 0)
		{
			for (var x = 0.5; x < this.canvas.width; x += this.grid_x)
			{
		  	this.context.moveTo(x, 0);
		  	this.context.lineTo(x, this.canvas.height);
			}
		}
		if(this.grid_y > 0)
		{
			for (var y = this.canvas.height - 0.5; y > 0; y -= this.grid_y)
			{
			  this.context.moveTo(0, y);
			  this.context.lineTo(this.canvas.width, y);
			}
		}
		this.context.strokeStyle = this.gridcolor;
		this.context.stroke();
		this.context.closePath();
	}
	/*this.checkPosition = function(pos_y)
	{
		if(pos_y < 0 )
		{
				pos_y = 0;
		}
		if(pos_y > this.maxvalue * this.scale)
		{
				pos_y = this.maxvalue * this.scale;
		}
		return pos_y;
	}*/

	/* Supplemental functions */
	this.toggleValue = function(name)
	{
		this[name] = this[name]? false:true;
		this.drawGraph();
	}
	this.setColor = function(newcolor)
	{
		this.strokeStyle = newcolor;
		this.drawGraph();
	}
	this.setFill = function(newfill)
	{
		this.fillStyle = newfill;
		this.drawGraph();
	}
	this.setValue = function(name,value)
	{
		this[name] = value;
		this.drawGraph();
	}
	this.toggleDraw = function()
	{
		if(this.timer == undefined)
		{
			this.timer = setInterval(draw, this.interval);
		}
		else
		{
			clearInterval(this.timer);
			this.timer = undefined;
		}
	}
	this.clearGraph = function()
	{
		this.array = [];
		this.drawGraph();
	}

	function draw()
	{
		_self.doGraph();
	}

	// If this is a dynamic graph, begin the timer, otherwise draw the graph
	if(this.data == undefined)
	{
		this.timer = setInterval(draw, this.interval);
		this.doGraph();
	}
	else
	{
		for(var i =0; i < this.data.length; i++)
		{
			this.array[i] = Math.round((this.range[1] - this.data[i] ) * this.scale); // Convert real values to graph values
		}	
		this.drawGraph();
	}
}